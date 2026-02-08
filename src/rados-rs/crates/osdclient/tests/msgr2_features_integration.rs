/// Integration tests for msgr2 protocol feature combinations
///
/// Tests that msgr2 features work correctly with live Ceph cluster.
/// The msgr2 feature negotiation happens automatically based on server capabilities.
///
/// These tests require a running Ceph cluster (use docker-compose or vstart.sh).
/// Run with: cargo test --test msgr2_features_integration -- --ignored --test-threads=1
///
/// NOTE: Currently tests use default msgr2 configuration which includes:
/// - Compression enabled (negotiated with server)
/// - SECURE mode preferred, fallback to CRC
/// - All msgr2 features advertised
///
/// Future enhancement: Expose msgr2::ConnectionConfig at MonClientConfig/OSDClientConfig level
/// to allow testing specific feature combinations.

use std::env;
use std::sync::Arc;
use uuid::Uuid;
use bytes::Bytes;
use denc::VersionedEncode;

/// Test configuration
struct TestConfig {
    mon_addrs: Vec<String>,
    keyring_path: String,
    entity_name: String,
    pool: String,
}

impl TestConfig {
    fn from_env() -> Self {
        let conf_path = env::var("CEPH_CONF").unwrap_or_else(|_| "/etc/ceph/ceph.conf".to_string());
        Self::from_ceph_conf(&conf_path)
            .unwrap_or_else(|e| panic!("Failed to load configuration from {}: {}", conf_path, e))
    }

    fn from_ceph_conf(path: &str) -> Result<Self, Box<dyn std::error::Error>> {
        let config = cephconfig::CephConfig::from_file(path)?;
        let mon_addrs = config.mon_addrs()?;
        let keyring_path = config.keyring()?;
        let entity_name = config.entity_name();
        let pool = env::var("CEPH_TEST_POOL").unwrap_or_else(|_| "test-pool".to_string());

        Ok(Self {
            mon_addrs,
            keyring_path,
            entity_name,
            pool,
        })
    }
}

/// Parse pool identifier to pool ID
async fn parse_pool(
    pool: &str,
    osd_client: &Arc<osdclient::OSDClient>,
) -> Result<u64, Box<dyn std::error::Error>> {
    if let Ok(id) = pool.parse::<u64>() {
        return Ok(id);
    }

    let osdmap = match osd_client.get_osdmap().await {
        Ok(map) => map,
        Err(_) => return Err("OSDMap not available".into()),
    };

    for (pool_id, pool_name) in &osdmap.pool_name {
        if pool_name == pool {
            return Ok(*pool_id);
        }
    }

    Err(format!("Pool '{}' not found", pool).into())
}

/// Setup test clients
async fn setup(
    test_name: &str,
) -> (Arc<monclient::MonClient>, Arc<osdclient::OSDClient>, u64) {
    println!("\n=== Testing: {} ===", test_name);

    let config = TestConfig::from_env();
    let map_notifier = Arc::new(osdclient::MapNotifier::new());

    // Create OSDMap receiver that decodes and posts to notifier
    struct OSDMapHandler {
        notifier: Arc<osdclient::MapNotifier<osdclient::OSDMap>>,
    }
    
    impl objecter::OSDMapReceiver for OSDMapHandler {
        fn handle_osdmap(&self, epoch: u32, data: bytes::Bytes) {
            let notifier = Arc::clone(&self.notifier);
            tokio::spawn(async move {
                match osdclient::OSDMap::decode_versioned(&mut data.as_ref(), 0) {
                    Ok(osdmap) => {
                        notifier.post(Arc::new(osdmap)).await;
                    }
                    Err(e) => {
                        eprintln!("Failed to decode OSDMap epoch {}: {}", epoch, e);
                    }
                }
            });
        }
    }
    
    let osdmap_receiver = Arc::new(OSDMapHandler {
        notifier: Arc::clone(&map_notifier),
    });

    // Create MonClient - it will forward OSDMaps to the receiver
    let mon_config = monclient::MonClientConfig {
        entity_name: config.entity_name.clone(),
        mon_addrs: config.mon_addrs.clone(),
        keyring_path: config.keyring_path.clone(),
        ..Default::default()
    };

    let mon_client = Arc::new(
        monclient::MonClient::new(mon_config, Some(osdmap_receiver))
            .await
            .expect("Failed to create MonClient"),
    );
    mon_client.init_self_ref();

    mon_client
        .init()
        .await
        .expect("Failed to initialize MonClient");

    mon_client
        .wait_for_auth(std::time::Duration::from_secs(5))
        .await
        .expect("Failed to complete authentication");

    tokio::time::sleep(tokio::time::Duration::from_millis(500)).await;

    // Create OSDClient
    let osd_config = osdclient::OSDClientConfig {
        entity_name: config.entity_name.clone(),
        ..Default::default()
    };

    let fsid = mon_client.get_fsid().await;

    let osd_client = osdclient::OSDClient::new(
        osd_config,
        fsid,
        Arc::clone(&mon_client),
        Arc::clone(&map_notifier),
    )
    .await
    .expect("Failed to create OSDClient");

    osd_client
        .clone()
        .start_osdmap_subscription()
        .await
        .expect("Failed to start OSDMap subscription");

    mon_client
        .subscribe("osdmap", 0, 0)
        .await
        .expect("Failed to subscribe to OSDMap");

    tokio::time::sleep(tokio::time::Duration::from_secs(2)).await;

    let pool_id = parse_pool(&config.pool, &osd_client)
        .await
        .expect("Failed to parse pool");

    println!("✓ Setup complete - pool_id={}", pool_id);

    (mon_client, osd_client, pool_id)
}

/// Test basic write/read operations
async fn test_operations(
    osd_client: &Arc<osdclient::OSDClient>,
    pool_id: u64,
    test_name: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    let object_name = format!("test-{}-{}", test_name, Uuid::new_v4());

    println!("  Testing write/read with object '{}'...", object_name);

    // Write small object
    let small_data = Bytes::from(format!("Test data for {}", test_name));
    osd_client.write_full(pool_id, &object_name, small_data.clone()).await?;
    println!("  ✓ Write small object successful");

    // Read back
    let read_result = osd_client.read(pool_id, &object_name, 0, 1024).await?;
    assert_eq!(read_result.data, small_data, "Data mismatch");
    println!("  ✓ Read successful, data matches");

    // Write large object (triggers compression if enabled)
    let large_object = format!("{}-large", object_name);
    let large_data = Bytes::from(vec![b'A'; 64 * 1024]); // 64KB - highly compressible
    osd_client.write_full(pool_id, &large_object, large_data.clone()).await?;
    println!("  ✓ Write large object successful (64KB)");

    // Read large object
    let read_large_result = osd_client.read(pool_id, &large_object, 0, 128 * 1024).await?;
    assert_eq!(read_large_result.data, large_data, "Large data mismatch");
    println!("  ✓ Read large object successful");

    // Write very large object to stress compression
    let very_large_object = format!("{}-very-large", object_name);
    let very_large_data = Bytes::from(vec![b'B'; 512 * 1024]); // 512KB
    osd_client.write_full(pool_id, &very_large_object, very_large_data.clone()).await?;
    println!("  ✓ Write very large object successful (512KB)");

    // Read very large object
    let read_very_large_result = osd_client.read(pool_id, &very_large_object, 0, 1024 * 1024).await?;
    assert_eq!(read_very_large_result.data, very_large_data, "Very large data mismatch");
    println!("  ✓ Read very large object successful");

    // Cleanup
    osd_client.delete(pool_id, &object_name).await?;
    osd_client.delete(pool_id, &large_object).await?;
    osd_client.delete(pool_id, &very_large_object).await?;
    println!("  ✓ Cleanup successful");

    Ok(())
}

//
// COMPREHENSIVE INTEGRATION TESTS
//

#[tokio::test]
#[ignore] // Requires live Ceph cluster
async fn test_msgr2_with_default_config() -> Result<(), Box<dyn std::error::Error>> {
    let (_mon_client, osd_client, pool_id) = setup("DefaultConfig").await;
    test_operations(&osd_client, pool_id, "DefaultConfig").await?;
    println!("✓✓✓ Default Configuration - ALL TESTS PASSED ✓✓✓\n");
    Ok(())
}

#[tokio::test]
#[ignore]
async fn test_msgr2_small_objects() -> Result<(), Box<dyn std::error::Error>> {
    let (_mon_client, osd_client, pool_id) = setup("SmallObjects").await;
    
    println!("  Testing many small objects...");
    for i in 0..10 {
        let object_name = format!("test-small-{}-{}", i, Uuid::new_v4());
        let data = Bytes::from(format!("Small object #{}", i));
        osd_client.write_full(pool_id, &object_name, data.clone()).await?;
        
        let read_result = osd_client.read(pool_id, &object_name, 0, 1024).await?;
        assert_eq!(read_result.data, data);
        
        osd_client.delete(pool_id, &object_name).await?;
    }
    println!("  ✓ 10 small objects written/read/deleted successfully");
    
    println!("✓✓✓ Small Objects Test - ALL TESTS PASSED ✓✓✓\n");
    Ok(())
}

#[tokio::test]
#[ignore]
async fn test_msgr2_large_objects() -> Result<(), Box<dyn std::error::Error>> {
    let (_mon_client, osd_client, pool_id) = setup("LargeObjects").await;
    
    println!("  Testing large objects (compression stress test)...");
    for i in 0..5 {
        let object_name = format!("test-large-{}-{}", i, Uuid::new_v4());
        // Create highly compressible data
        let data = Bytes::from(vec![b'X'; 256 * 1024]); // 256KB of same byte
        osd_client.write_full(pool_id, &object_name, data.clone()).await?;
        
        let read_result = osd_client.read(pool_id, &object_name, 0, 512 * 1024).await?;
        assert_eq!(read_result.data, data);
        
        osd_client.delete(pool_id, &object_name).await?;
    }
    println!("  ✓ 5 large compressible objects written/read/deleted successfully");
    
    println!("✓✓✓ Large Objects Test - ALL TESTS PASSED ✓✓✓\n");
    Ok(())
}

#[tokio::test]
#[ignore]
async fn test_msgr2_mixed_objects() -> Result<(), Box<dyn std::error::Error>> {
    let (_mon_client, osd_client, pool_id) = setup("MixedObjects").await;
    
    println!("  Testing mixed object sizes...");
    let sizes = vec![1u64, 100, 1024, 4096, 16384, 65536, 262144]; // 1B to 256KB
    
    for size in sizes {
        let object_name = format!("test-mixed-{}-{}", size, Uuid::new_v4());
        let data = Bytes::from(vec![b'M'; size as usize]);
        osd_client.write_full(pool_id, &object_name, data.clone()).await?;
        
        let read_result = osd_client.read(pool_id, &object_name, 0, size * 2).await?;
        assert_eq!(read_result.data, data);
        
        osd_client.delete(pool_id, &object_name).await?;
        println!("  ✓ Object size {} bytes OK", size);
    }
    
    println!("✓✓✓ Mixed Objects Test - ALL TESTS PASSED ✓✓✓\n");
    Ok(())
}

#[tokio::test]
#[ignore]
async fn test_msgr2_concurrent_operations() -> Result<(), Box<dyn std::error::Error>> {
    let (_mon_client, osd_client, pool_id) = setup("ConcurrentOps").await;
    
    println!("  Testing concurrent operations...");
    let mut handles = vec![];
    
    for i in 0..10 {
        let osd_client = Arc::clone(&osd_client);
        let handle = tokio::spawn(async move {
            let object_name = format!("test-concurrent-{}-{}", i, Uuid::new_v4());
            let data = Bytes::from(format!("Concurrent test #{}", i));
            
            osd_client.write_full(pool_id, &object_name, data.clone()).await?;
            let read_result = osd_client.read(pool_id, &object_name, 0, 1024).await?;
            assert_eq!(read_result.data, data);
            osd_client.delete(pool_id, &object_name).await?;
            
            Ok::<_, denc::RadosError>(())
        });
        handles.push(handle);
    }
    
    for handle in handles {
        match handle.await {
            Ok(Ok(())) => {},
            Ok(Err(e)) => return Err(format!("Operation failed: {}", e).into()),
            Err(e) => return Err(format!("Task panicked: {}", e).into()),
        }
    }
    
    println!("  ✓ 10 concurrent operations completed successfully");
    println!("✓✓✓ Concurrent Operations Test - ALL TESTS PASSED ✓✓✓\n");
    Ok(())
}
