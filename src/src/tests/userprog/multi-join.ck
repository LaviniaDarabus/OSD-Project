# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(multi-join) begin
(multi-join) Child about to sleep
(multi-join) Child about to terminate
(multi-join) Child finished
(multi-join) end
multi-join: exit(0)
EOF
pass;
