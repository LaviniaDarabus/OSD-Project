# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(multi-joinall) begin
(multi-joinall) Thread about to terminate
(multi-joinall) Thread about to terminate
(multi-joinall) Main is finishing
(multi-joinall) end
multi-joinall: exit(0)
EOF
pass;
