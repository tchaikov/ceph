#pragma once

#include "pg_backend.h"

class ReplicatedBackend : public PGBackend {
public:
  seastar::future<bool> handle_message(Ref<Message> m);
};
