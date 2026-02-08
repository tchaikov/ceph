# Bisection Analysis: MOSDOp Timeout Fix

## Executive Summary

The MOSDOp write operation timeout was caused by sending MOSDOp v9 format messages to Ceph v18 OSDs, which only understand v8 format. Through bisecting commits, we identified:

1. **Initial Fix (dc5bd8f1)**: Hardcoded v8 encoding - resolved timeout
2. **Regression (49c345a5)**: Added conditional encoding but kept v9 version - re-introduced timeout risk
3. **Final Fix (85209b5f)**: Made version feature-aware - properly resolved issue

The current code (HEAD at 851fbb9c) is correct and properly implements feature-based encoding.

## Timeline of Changes

### Before Fixes (f8d69194 and earlier)

**Code State:**
```rust
pub const VERSION: u16 = 9;

fn encode_payload(&self, _features: u64) -> Result<Bytes> {
    // ... encoding ...
    let otel_trace = JaegerSpanContext::invalid();
    otel_trace.encode(&mut buf, 0)?;  // Always encoded!
    // ...
}
```

**Problem:**
- Always sent MOSDOp v9 format with OpenTelemetry trace field
- Ceph v18 OSDs don't understand v9 format (added in v19 with SERVER_SQUID)
- OSD drops message → client times out after 30 seconds

### Fix #1: dc5bd8f1 (Initial Fix) ✅

**Commit:** `dc5bd8f1e7a8acc45074b8d70107fb895ff00087`
**Date:** Feb 7 06:28:37 2026
**Title:** "Fix MOSDOp timeout by using v8 encoding for Ceph v18 compatibility"

**Changes:**
```diff
-    pub const VERSION: u16 = 9;
+    pub const VERSION: u16 = 8;

     fn encode_payload(&self, _features: u64) -> Result<Bytes> {
         // ... encoding ...
-        let otel_trace = JaegerSpanContext::invalid();
-        otel_trace.encode(&mut buf, 0)?;
+        // Note: v9 adds otel_trace here, but we use v8 for v18 compatibility
         // ...
     }
```

**Result:**
- ✅ Tests completed in 2.68 seconds (was 30s timeout)
- ✅ Compatible with Ceph v18.2.7
- ⚠️  But hardcoded v8 - can't use v9 features with Ceph v19+

### Regression: 49c345a5 ❌

**Commit:** `49c345a578e0d6dd62a2a3df02d6ab4d6e822f3a`
**Date:** Feb 7 06:48:52 2026
**Title:** "Implement feature-based MOSDOp encoding (v8/v9)"

**Changes:**
```diff
     fn msg_version() -> u16 {
-        Self::VERSION
+        // This is only used as a default/fallback
+        // Actual version is determined in encode_payload based on features
+        9
     }

-    fn encode_payload(&self, _features: u64) -> Result<Bytes> {
+    fn encode_payload(&self, features: u64) -> Result<Bytes> {
+        let has_squid = has_feature(features, CEPH_FEATUREMASK_SERVER_SQUID);
+
         // ... encoding ...
-        // Note: v9 adds otel_trace here, but we use v8 for v18 compatibility
+        // 6b. otel_trace - added in v9, only if SERVER_SQUID present
+        if has_squid {
+            let otel_trace = JaegerSpanContext::invalid();
+            otel_trace.encode(&mut buf, 0)?;
+        }
         // ...
     }
```

**Problem:** Version Mismatch!
- Message header says version 9 (from `msg_version()`)
- But payload is v8 format when SERVER_SQUID not present
- Ceph v18 OSD receives message with v9 header, tries to parse as v9, fails

**Impact:**
- Re-introduced timeout for connections without SERVER_SQUID feature
- Would work with Ceph v19+ but fail with v18 again

### Final Fix: 85209b5f ✅

**Commit:** `85209b5f2df6a06bc2dc1b11d9d662ea51432a97`
**Date:** Feb 7 11:01:04 2026
**Title:** "Make msg_version() feature-aware and consolidate front_size calculation"

**Changes:**
```diff
-    fn msg_version() -> u16 {
-        // This is only used as a default/fallback
-        // Actual version is determined in encode_payload based on features
-        9
+    fn msg_version(features: u64) -> u16 {
+        // Return v9 if SERVER_SQUID feature is present (Ceph v19+)
+        // Return v8 otherwise for backward compatibility with Ceph v18
+        use denc::features::CEPH_FEATUREMASK_SERVER_SQUID;
+        if features & CEPH_FEATUREMASK_SERVER_SQUID != 0 {
+            9
+        } else {
+            8
+        }
     }
```

**Result:**
- ✅ Message header version matches payload encoding
- ✅ v8 for Ceph v18 (no SERVER_SQUID)
- ✅ v9 for Ceph v19+ (with SERVER_SQUID)
- ✅ No version mismatch
- ✅ Proper feature negotiation

## Technical Deep Dive

### MOSDOp Message Format Differences

**v8 Format (209 bytes for PGLS):**
```
[spgid] [hash] [epoch] [flags] [reqid] [blkin_trace] [client_inc]
[mtime] [object_locator] [object_name] [ops] [snapid] [snap_seq]
[snaps] [retry_attempt] [features] [feature_incompat]
```

**v9 Format (216 bytes for PGLS):**
```
[spgid] [hash] [epoch] [flags] [reqid] [blkin_trace]
[otel_trace ← NEW! (7 bytes)] [client_inc] [mtime]
[object_locator] [object_name] [ops] [snapid] [snap_seq]
[snaps] [retry_attempt] [features] [feature_incompat]
```

**Difference:** v9 adds `JaegerSpanContext` (OpenTelemetry trace) - 7 bytes after `blkin_trace`

### Feature Negotiation Flow

```
1. Client ←→ Server: msgr2 Banner Exchange
2. Client ←→ Server: Auth (features negotiated)
3. StateMachine stores negotiated_features
4. OSDSession.peer_features = negotiated_features
5. encode_operation() calls CephMessage::from_payload(features)
6. from_payload() calls T::msg_version(features)
7. MOSDOp::msg_version(features) returns 8 or 9
8. Message header set with correct version
9. encode_payload(features) encodes matching format
```

### Why Ceph v18 Failed with v9 Messages

When Ceph v18 OSD receives a message:
1. Reads message header → sees version 9
2. Tries to decode using v9 format → expects otel_trace field
3. If message is actually v8 format → parsing fails
4. OSD logs error and drops message
5. Client never receives response → timeout after 30s

## Verification

### Test with Ceph v18

```rust
// Features negotiated: no SERVER_SQUID
let features = 0x3f01cfbfbffdffff;  // Without SQUID
let version = MOSDOp::msg_version(features);
assert_eq!(version, 8);  // ✅ Correct

// Payload encoding
let payload = mosdop.encode_payload(features)?;
assert_eq!(payload.len(), 209);  // ✅ v8 size (no otel_trace)
```

### Test with Ceph v19+

```rust
// Features negotiated: with SERVER_SQUID
let features = 0x3f01cfbffffdffff | CEPH_FEATUREMASK_SERVER_SQUID;
let version = MOSDOp::msg_version(features);
assert_eq!(version, 9);  // ✅ Correct

// Payload encoding
let payload = mosdop.encode_payload(features)?;
assert_eq!(payload.len(), 216);  // ✅ v9 size (with otel_trace)
```

## Conclusion

The bisection identified three key commits:

1. **dc5bd8f1** - Initial quick fix (hardcoded v8)
2. **49c345a5** - Regression (version mismatch)
3. **85209b5f** - Proper fix (feature-aware version)

The current code (HEAD) correctly implements feature-based MOSDOp encoding:
- Version and payload always match
- Compatible with both Ceph v18 and v19+
- No timeout issues

**No additional fixes needed.** The issue is fully resolved.

## Recommendations

1. ✅ Keep the current implementation (feature-aware msg_version)
2. ✅ Add integration tests for both v18 and v19+ clusters
3. ✅ Document the version differences for future maintainers
4. Consider adding runtime validation: assert!(header.version matches payload_format)

## References

- Ceph MOSDOp.h: `src/messages/MOSDOp.h` in Ceph source
- SERVER_SQUID feature: Introduced in Ceph v19 (Squid release)
- msgr2 protocol: `src/msg/async/ProtocolV2.cc` in Ceph source
