# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(multi-crit) begin
(multi-crit) end
multi-crit: exit(0)
EOF
pass;