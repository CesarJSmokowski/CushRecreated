Student Information
-------------------
Cesar Smokowski (cesar8800@vt.edu)

How to execute the shell
------------------------
My shell can be executed by calling ./cush while in the src directory

Important Notes
---------------
My custom shell works with all of the built-in and non-built in commands. It also supports \^C, \^Z, pipes, I/O redirection and exclusive access. 
The two custom built-ins I chose to implement were an interesting fact generator, called with "fact [category]", and two command line history functions 
set up using the GNU History Library. The user can call "history" to view their command line history and reset the command line history with "clear_history".

I created the two test files history_test.py and interesting_fact_test.py in the src directory to test the functionality of my two custom built-ins. Both the fact [category]
and history built-ins were thoroughly tested. All of the categories, including non-valid categoires, were tested for the interesting fact function. Printing the command line history with history(),
clearing the history list with clear_history, using the arrow keys and the use of event designators were all tested for the history built in. 

history_test.py and interesting_fact_test.py can be run by calling "stdriver.py -v custom_tests.tst" in the src directory.



Description of Base Functionality
---------------------------------
I implemented the built-in commands jobs, fg, bg, kill and stop by creating helper functions that are called from my
main parse_pipelines function when one of those jobs is detected. The fg, bg, kill and stop helper functions were
implemented similarly, by taking in the current command struct object as a parameter, checking if the the command->argv[1]
is not null then checking if the job the user is looking for exists in the job_list. Once we verify that the job exists, we
spend the appropriate signal based on it's status and whether we want to move it to the foreground, backgroup, stop or kill it.

The jobs command was implemented by iterating through each of the job struct objects in our job_list, which contains only our processes that 
are still alive, and printing out each one with the print_job function.

To get \ˆC and \ˆZ set up, I created two functions, sigint_handler for dealing with Ctrl-C and sigtstp_handler for when Ctrl-Z is called. Both
of these functions are passed into signal_set_handler with their respective signals, SIGINT with sigint_handler and SIGTSTP with sigtstp_handler.
Sigtstp_handler and sigint_handler are set up very similarly, where we are iterating through the job_list and checking if there is a job in the foreground.
If there is a job with FOREGROUND status, we call killpg with the SIGINT signal to terminate the foreground process. We do the same thing except call
killpg with SIGTSTP to stop the foreground process in sigtstp_handler. Implementing sigtstp_handler and sigint_handler allows our shell to handle /^C and /^Z
commands correctly and send the appropriate signal to the foreground process, if there is one.


Description of Extended Functionality
-------------------------------------
Handling pipes, exclusive access and I/O redirection is all done in my main parse_pipelines method. I set up
piping by creating two pipes, afterPipe and beforePipe, for each command in our ast_pipeline. In order to set the right
values for the read and write values in both of our pipes, I split my implementation into three cases: our current command is either 
the first command in the pipeline, one of the middle commands or the last command. My implementation for each condition is listed below:

1) Middle command:
	First thing we do is use posix_spawn_file_actions_adddup2 to set the read value of the before pipe equal to standard input
	Next we initalize the afterPipe with pipe2(afterPipe, O_CLOEXEC
	Lastly, we sent the write value of afterPipe (afterPipe[1]) equal to standard output using posix_spawn_file_actions_adddup2

2) First Command:
	We first check if our pip->iored_input != null. If it does not, we call posix_spawn_file_actions_addopen with the O_RDONLY and S_IRWXO parameters
	Next we check if our command is not equal to the last command in our pipeline, which means there is more than one command in the pipeline. If the 
		current command does not equal the last command in the pipeline, then we initialize the afterPipe with pipe2(afterPipe, O_CLOEXEC);
	Lastly, we sent the write value of afterPipe to standard out with posix_spawn_file_actions_adddup2()

3) Last Command:
	We check if the pipe->iored_output value is NULL. If it is not, then we check if the pipe->append_to_output boolean value is true
		if pipe->append_to_output is true, it means we need to write to the end of a file instead of overwriting it. If pipe->append_to_output is true, 
		then we call posix_spawn_file_actions_addopen with the O_WRONLY, O_CREAT and O_APPEND flags
	If pipe->append_to_output is false, then we call posix_spawn_file_actions_addopen with the O_WRONLY | O_CREAT flags
	Lastly, we check if our current command is the first command in the pipeline, which means there is only one command in the pipeline.
		if this is true, then we set the read value of beforePipe (beforePipe[0]) equal to standard input with posix_spawn_file_actions_adddup2


Once we get through these three cases, we call posix_spawnattr_setflags, posix_spawnattr_setpgroup and posix_spawnp then close the appropriate pipe values based 
on if the command is the first command in the pipeline, a middle command or the last command. Setting up my implementation this way made it much easier to understand
what values needed to be stored in each pipe and all of the conditions we needed to check to get the I/O redirection and exclusive access set up.
	

List of Additional Builtins Implemented
---------------------------------------

Function name: fact [category]
I chose to implement an interesting fact generator for the simple built in function. When the user calls "fact" followed
by either a specific category from the list {animals, food, computers, sports, history, movies, politics}, the shell will output an interesting fact
associated with that topic. If the user calls fact with no second argument or something not from the list of categories, the shell will output
a completely randomized interesting fact. I implemented this function as a way to practice generating randomized outputs and it's a fun, engaging 
addition to the custom shell. An example of the fact command being called several different ways is show below:
	
	cush> fact food
	Interesting Food Fact: Strawberries are not actually berries since their seeds are on the outside
	cush> fact computers
	Interesting Computer Fact: The First Computer Weighed More Than 27 Tons
	cush> fact sports
	Interesting Sports Fact: The average life span of an MLB baseball is five to seven pitches
	cush> fact history
	Interesting History Fact: Cleopatra, the last Queen of Egypt, was not Egyptian. She was Greek
	cush> fact movies
	Interesting Movie Fact: Alfred Hitchcock's Psycho was the first U.S. film to feature a toilet flusing
	cush> fact politics
	Interesting Politics Fact: Approximately 15,000 books have been written about Abraham Lincoln
	cush> fact random
	Interesting Food Fact: Strawberries are not actually berries since their seeds are on the outside
	cush> fact abc
	Interesting History Fact: Cleopatra, the last Queen of Egypt, was not Egyptian. She was Greek



Function name: history and clear_history
	For the more complex custom built in, I created two built in functions that store the command list history for our shell
	and let the user clear it at any time using the GNU History Library. By calling "history", the shell will output a list of the command line inputs given to the 
	shell. The user can also use their arrow keys to cycle through their command history if they want to call a previous command again. An example of the "history"
	and "clear_history" commands at work is shown below:
	
		cush> echo hello
		hello
		cush> kill 2
		kill 2: No such job
		cush> jobs
		cush> sleep 5
		^Z[1]   Stopped         (sleep 5)
		cush> kill 1
		cush> history
		0: echo hello
		1: kill 2
		2: jobs
		3: sleep 5
		4: kill 1
		cush> clear_history
		cush> sleep 1
		cush> history
		0: sleep 1
		cush> 

	Our custom shell's history implementation also supports events designators. Event designators incldue 
	!, !n, !-n, !!, !string, !?string[?], ^string1^string2^, !#. Referencing a event designator is a simple 
	way to execute or modify a command that was previous called in the shell. 







