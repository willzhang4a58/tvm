# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
import pytest

pytest.importorskip("ethosu.vela")

import tvm.contrib.ethosu.cascader as pl
from tvm.contrib.ethosu.cascader.parts import EthosuPart


def test_ethosu_part():
    te_subgraph = pl.TESubgraph([], None)
    output_quantum = [1, 2, 2, 8]
    quantum_cycles = 32
    propagator = pl.Propagator(
        [[1, 0, 0, 0, 2], [0, 1, 0, 0, 2], [0, 0, 1, 0, 0], [0, 0, 0, 1, 0], [0, 0, 0, 0, 1]],
        [0, 0, 0, 0],
    )
    stripe_config = pl.StripeConfig(
        [1, 4, 4, 16], [1, 64, 72, 96], [1, 4, 4, 16], [1, 2, 3, 4], [1, 16, 13, 6], [0, 0, 0, 0]
    )

    part = EthosuPart(te_subgraph, [propagator], output_quantum, quantum_cycles)

    assert part.get_stripe_align_hint() == output_quantum
    # Check that the performance model runs, don't verify output
    part.get_performance_info(stripe_config, False)
    part.get_performance_info(stripe_config, True)


if __name__ == "__main__":
    pytest.main([__file__])
