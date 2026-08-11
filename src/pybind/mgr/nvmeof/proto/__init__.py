# -*- coding: utf-8 -*-
# The checked-in bindings predate protobuf 3.20; newer runtimes only
# load them with the pure-python implementation. Select it before any
# google.protobuf import can happen. This goes away once the bindings
# are generated at build time.
import os

os.environ.setdefault("PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION", "python")
