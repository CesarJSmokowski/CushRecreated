#
# fg_test: tests the fg command
# 
# Test the fact command to print out an interesting fact given a specific category or one will be chosen at random
# Requires the following commands to be implemented
# or otherwise usable:
#
#	fact [animals, food, computers, sports, history, movies or random]
#

#from re import RegexFlag
import sys, imp, atexit, pexpect, proc_check, signal, time, threading
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

# run "fact animals" command
sendline("fact animals")
expect_regex(r'(Interesting).+(Fact).+')

sendline("fact animals")
expect_exact("Interesting Animal Fact: A crocodile cannot stick it's tongue out", "fact animals was not correct")

sendline("fact food")
expect_exact("Interesting Food Fact: Strawberries are not actually berries since their seeds are on the outside", "fact food was not correct")

#test out running the "fact computers" command. Computers is one of the six valid categories. If a non-valid category is passed, a random fact is output
sendline("fact computers")
expect_exact("Interesting Computer Fact: The First Computer Weighed More Than 27 Tons", "fact food was not correct")

#test out calling the "fact sports" command. We know the exact output here, because a valid category is provided as the second argument
sendline("fact sports")
expect_exact("Interesting Sports Fact: The average life span of an MLB baseball is five to seven pitches")

sendline("fact history")
expect_exact("Interesting History Fact: Cleopatra, the last Queen of Egypt, was not Egyptian. She was Greek")

sendline("fact movies")
expect_exact("Interesting Movie Fact: Alfred Hitchcock's Psycho was the first U.S. film to feature a toilet flusing")

sendline("fact politics")
expect_exact("Interesting Politics Fact: Approximately 15,000 books have been written about Abraham Lincoln")

#We want to test the case where the user gives facts a random, non-valid category value. 
#This will generate a random fact from the categories we defined
sendline("fact abc")
expect_regex(r'(Interesting).+(Fact).+')

sendline("fact random")
expect_regex(r'(Interesting).+(Fact).+')

sendline("exit")
expect_exact("exit\r\n", "Shell output extraneous characters")

test_success()