# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(multi-pid) begin
(multi-pid) end
multi-pid: exit(0)
EOF
pass;
