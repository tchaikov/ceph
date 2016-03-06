// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_TEST_RBD_MIRROR_TEST_MOCK_FIXTURE_H
#define CEPH_TEST_RBD_MIRROR_TEST_MOCK_FIXTURE_H

#include "test/rbd_mirror/test_fixture.h"
#include <boost/shared_ptr.hpp>
#include <gmock/gmock.h>

namespace librados {
class TestRadosClient;
class MockTestMemIoCtxImpl;
class MockTestMemRadosClient;
}

namespace rbd {
namespace mirror {

class TestMockFixture : public TestFixture {
public:
  typedef boost::shared_ptr<librados::TestRadosClient> TestRadosClientPtr;

  static void SetUpTestCase();
  static void TearDownTestCase();

  virtual void SetUp();
  virtual void TearDown();

  ::testing::NiceMock<librados::MockTestMemRadosClient> &get_mock_rados_client() {
    return *s_mock_rados_client;
  }
  librados::MockTestMemIoCtxImpl &get_mock_io_ctx(librados::IoCtx &ioctx);

private:
  static TestRadosClientPtr s_test_rados_client;
  static ::testing::NiceMock<librados::MockTestMemRadosClient> *s_mock_rados_client;
};

} // namespace mirror
} // namespace rbd

#endif // CEPH_TEST_RBD_MIRROR_TEST_MOCK_FIXTURE_H
