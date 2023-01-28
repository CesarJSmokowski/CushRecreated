#
# history_test: tests the history command
# 
# Test the print, clear and add_history functions
# Requires the following commands to be implemented
# or otherwise usable:
#
#	clearHistory(), printHistory()
#

import sys, imp, atexit, pexpect, proc_check, signal, time, threading
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()