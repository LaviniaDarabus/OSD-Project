# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(multi-exit-m) begin
(multi-exit-m) end
multi-exit-m: exit(0)
EOF
pass;