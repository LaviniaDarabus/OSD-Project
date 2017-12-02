# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(multi-tid) begin
(multi-tid) end
multi-tid: exit(0)
EOF
pass;
