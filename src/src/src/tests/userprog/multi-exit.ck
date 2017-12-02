# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(multi-exit) begin
(multi-exit) end
multi-exit: exit(0)
EOF
pass;
