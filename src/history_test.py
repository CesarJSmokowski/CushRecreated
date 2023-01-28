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

sendline("ls")
sendline("kill 1")
sendline("history")

expect_exact("0: ls\r\n1: kill 1", "history was not correct")

#Now we test the functionality that clear_history will reset the history list
sendline("clear_history")
sendline("kill 4")
sendline("history")

expect_exact("0: kill 4", "clear_history function did not work")

up = "\x1b[A"
down = "\x1b[B"

#Testing arrow up: by calling up = "\x1b[A" we check that we can access previous commands with
#the arrow keys
sendline(up)
expect_exact("kill 4: No such job", "history was not correct")

# Event Designators "!" tests
# Here we are verifying that we can execute commands in our history list with the "!" event designator
sendline("echo test !")
sendline("!echo")
expect_exact("test !")

sendline("kill 2")
sendline("echo we're testing event designators")
sendline("stop 7")
sendline("ls")
sendline("!kill")
expect_exact("kill 2: No such job", "Calling !kill did not output what was expect")

# Event Designator "!n": n is an integer that specifies a command lines number for a previous
# command that the user wants to execute

sendline("clear_history")
sendline("kill 1")
sendline("echo testing !n")
sendline("jobs")
sendline("fg 6")
sendline("fact animals")
sendline("!1")
expect_exact("kill 1: No such job", "Calling !1 did not output what was expected")

# Event Designator "!-n": Running !-n will execute the command n lines back

sendline("clear_history")
sendline("ls")
sendline("jobs")
sendline("fg 4")
sendline("echo testing event designators")
sendline("stop 2")
sendline("history") #history is not added to the history_list
sendline("fact animals")
sendline("!-3")
expect_exact("testing event designators", "Calling !-3 did not output what was expected")

# Event Designator "!!". Calling "!!" will execute the previous command. This is equivalent to "!-1"
sendline("echo sunrise")
sendline("history")
sendline("stop 1")
sendline("ls")
sendline("fact computers")
sendline("!!")
expect_exact("Interesting Computer Fact: The First Computer Weighed More Than 27 Tons", "Calling !! did not output what was expect")

# Event Designator "^string1^string2^". Calling "^string1^string2^" will replace string1 with string2 and execute the command containing
# string1
sendline("clear_history")
sendline("fact movies")
sendline("ls")
sendline("echo fall 2021 Cesar Smokowski")
sendline("echo hello")
sendline("^hello^goodbye^")
expect_exact("goodbye", "String substition did not have expected output")





