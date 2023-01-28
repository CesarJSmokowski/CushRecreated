/*
 * Intro to Computer Systems Project 1
 * By Cesar Smokowski
 * 
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <signal.h>
#include "signal_support.h"

#include <string.h>
#include <wait.h>
#include <errno.h>
#include "../posix_spawn/spawn.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <readline/history.h>
#include <sys/wait.h>


sigset_t mask, omask;

#define errExit(msg)    do { perror(msg); \
                                    exit(EXIT_FAILURE); } while (0)

#define errExitEN(en, msg) \
                        do { errno = en; perror(msg); \
                                    exit(EXIT_FAILURE); } while (0)



/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

static void handle_child_status(pid_t pid, int status);

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The stavte of the terminal when this job was 
                                        stopped after having been in foreground */
    int processGroupID;      
    //int pid;   
    struct list pidList;                  

    /* Add additional fields here if needed. */
};

//------------- History Command Code ----------------
//We are establishing the list of HIST_ENTRY* objects that will store the user's comamnd line history
HIST_ENTRY **the_history_list;

//------------ End of History Code ------------------

//pid struct is used for the pid_list attribute of the job struct
struct pid {
    struct list_elem elem;
    pid_t pidValue;

};

//The jid is the index in our job list
//pid is the large number assigned by the operating

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job * jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job * 
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    job->processGroupID = 0;
    if (pipe->bg_job) {
        job->status = BACKGROUND;
    }
    else {
        job->status = FOREGROUND;
    }

    list_init(&job->pidList);
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

//Returns a string responding to the status
//of a job
static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}


//Ctrl-C
//This is the function that handles when Ctrl-c is called
//Ctrl-c is called with the purpose of terminating the job in the forground
//we iterate through the job list and send the SIGINT signal to the job in the 
//forgroud if there is one
static void sigint_handler(int sig, siginfo_t *info, void *_ctxt)
{

    assert(sig == SIGINT);

    //bool foundForeground = false;

    for (struct list_elem * e = list_begin(&job_list); 
         e != list_end(&job_list); 
         e = list_next(e)) {

             struct job *currJob = list_entry(e, struct job, elem);

             if (currJob->status == FOREGROUND) {
                 killpg(currJob->processGroupID, SIGINT);
                 //We send the SIGINT signal to the job we found in the foreground
             }


            //You do not need to send the signal back
            //if there are no thing in the foreground, just exit

         }

}

//Ctrl-Z
//This function handles the case where Ctrl-Z is called
//Ctrl-Z is called with the purpose of stopping the job in the foreground
//We need to iterate through the job list and check if there is a job in the foreground
//If there is a forground job, send the SIGTSTP signal to it and change it's status to stopped
static void sigtstp_handler(int sig, siginfo_t *info, void *_ctxt)
{

    assert(sig == SIGTSTP);

    bool foundForeground = false;

    for (struct list_elem * e = list_begin(&job_list); 
         e != list_end(&job_list); 
         e = list_next(e)) {

             struct job *currJob = list_entry(e, struct job, elem);

             if (currJob->status == FOREGROUND) {
                 //should I manually set the currJob->status to
                 //or should I directly call handle_chld_status which could change the status of the job
                 currJob->status = STOPPED;
                 killpg(currJob->processGroupID, SIGTSTP);
                 foundForeground = true;

             }


            //You do not need to send the signal back
            //if there are no thing in the foreground, just exit
            //

         }

         if (!foundForeground) {
            exit(0);
         }

}


//siginthandler, r SIGTSTP,
//we only want to 
//If there is something in the foreground, call killpg. If there is no foreground then exit


/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}

//This function returns a job that corresponds to the pid parameter value
static struct job * get_job_from_pid(int pid) {
    for (struct list_elem * e = list_begin(&job_list); 
         e != list_end(&job_list); 
         e = list_next(e)) {
        struct job *currJob = list_entry(e, struct job, elem);


        //go through each element in pidList to checkd
        for (struct list_elem * currPid = list_begin(&currJob->pidList); 
         currPid != list_end(&currJob->pidList); 
         currPid = list_next(currPid)) {

            struct pid *checkPid = list_entry(currPid, struct pid, elem);
            //you should be blocking sigchild everytime you access this function
            if (checkPid->pidValue == pid) {
                return currJob;
            }

         }


        
        //ast_command_print(job);
        }
    return NULL;
}



/*
Event                                   How to check for it         Additional info                 Process stopped?            Process dead?
User stops fg process with Ctrl-z       WIFSTOPPED                  WSTOPSIG equals SIGSTP          yes                         no
user stops process with kill -STOP      WIFSTOPPED                  STOPSIG equals SIGSTOP          yes                         no
non-foreground process wants            WIFSTOPPED                  WSTOPSIG equals SIGTTOU         yes                         no
 terminal access                                                    or SIGTTIN
process exits via exit()                WIFEXITED                   WEXITEDSTATUS has return code   no                          yes
user terminates process with Ctrl-C     WIFSIGNALED                 WTERMSIG equals SIGINT          no                          yes
User terminates process with kill       WIFSIGNALED                 WTERMSIG equals SIGTERM         no                          yes
user terminates process with kill -9    WIFSIGNALED                 WTERMSIG equals SIGKILL         no                          yes
Process has been terminated             WIFSIGNALED                 WTERMSIG equals signal number   no                          yes
(general case)
*/

//Handle child status handles the cases where an error occurs or a signal is sent 
//in the program.
static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    //handle_child_status gets automatically called when a child signal changes or when wait_for_job is called

    //loop through your job list and find which one has the right pid
    //job does not exist
    //you can do get_job_from_pid

    

    struct job *job = get_job_from_pid(pid);

    //print out error codes, segmentation fault, aborted, killed, floating point exception, killed, terminated

    bool wifStooped = WIFSTOPPED(status); //stopped
    bool wifexited = WIFEXITED(status); //exited
    bool wifsignal = WIFSIGNALED(status); //terminated

    if (wifsignal) { //WIFSTOPPED is true

        //user terminates process with Ctrl-C
        //User terminates process with kill
        //user terminates process with kill -9
        //Process has been terminated (general case)

        int signal = WTERMSIG(status);

        switch (signal) {
            case SIGSEGV:
                fprintf(stderr, "segmentation fault\n");
                break;
            case SIGABRT:
                fprintf(stderr, "aborted\n");
                break;
            case SIGFPE:
                fprintf(stderr, "floating point exception\n");
                break;
            case SIGKILL:
                fprintf(stderr, "killed\n");
                break;
            case SIGTERM:
                fprintf(stderr, "terminated\n");
                break;
            //default:
                //No error was found SIGTERM
        
    }

        //we do not need to call killpg

        int sig = WTERMSIG(status);
        //job->num_processes_alive--;
        //iterate over the pidlist once, then remove the matching pid then free it

        if (sig == SIGINT) {
            //Ctr-c is called, nothing special for SIGINT
            //fprintf(stderr, "^C");
            job->num_processes_alive--;

            if (job->num_processes_alive == 0) {
                //delete_job(job);
            }
            //the process is dead so update num_processes_alive
            //we only delete the job if there are no processes alive 
        }
        else if (sig == SIGTERM) {
            //User terminates process with kill
            //killpg(job->processGroupID, ); //kill the current process ??
            fprintf(stderr, "Terminated");
            job->num_processes_alive--;
            


        }
        else {
            //WTERMSIG equals signal number
            job->num_processes_alive--;
            
        }
    }
    if (wifStooped) {

        //we just need to change the job status 

        //User stops fg process with Ctrl-z, WSTOPSIG equals SIGSTP, 
        //user stops process with kill -STOP, WSTOPSIG equals SIGSTOP
        //non-foreground process wants terminal access

        int sig = WSTOPSIG(status);
        int sig2 = WSTOPSIG(status);
        if (sig == SIGTSTP || sig2 == SIGSTOP) {
            //User stops fg process with Ctrl-z
            job->status = STOPPED;

            //**** save state here ******
            //we should be calling termstate_save_sample

            termstate_save(&job->saved_tty_state);
            termstate_give_terminal_back_to_shell();
                print_job(job);

        }
        if (sig == SIGTTOU || sig == SIGTTIN) {
            //non-foreground process wants terminal access
            //***** save state here ********************
            //termstate_save(&job->saved_tty_state);
            job->status = NEEDSTERMINAL;

        }
    }

    if (wifexited) {
        if (job->num_processes_alive > 0) {
            job->num_processes_alive--; //We are running into a segmentation fault here with job 
        }

        if (job->status == FOREGROUND) {
            termstate_sample();
        }

        
    }

}

    //Copy syntax from 
    //char **argv; from shell-ast.h/struct ast_command is the array of strings that we want to parse through
    //and figure out what we want to execute

    //iterate through all the pipelines for the ast_command_line struct
    //iterate through the commands, fork a process and have them output to the shell
    //for each command, fork a process and have them execute it
    //there are built ins and not built ins
    //non built ins just make a new child process, then go find the ls or other file
    //When the shell sees a built in, it runs the kill process 

    //function for each of the built ins
    //execve will be called to find the file for a built in ast_command
    //pausexspawn or fork then execve() for built in file

    //make an array of strings that contain the built in names
    //If I detect a built in in the command line, then just run the built in and ignore the rest

    //have a function that checks if it is a built in.
    //once you find a built in, execute it and break
    //Check the whole pipeline, ignore the rest 
    //stop the current pipeline then move on to the next pipeline

    //e built-in commands ‘jobs,’ ‘fg,’ ‘bg,’ ‘kill,’ ’exit,’ and ‘stop.’ 


    //---------------- Custom Job functions ------------------------

    //this function returns a randomized integer within the range of the lower and upperValue
    static int calculateRandomValue(int lowerValue, int upperValue, int num)
    {
    int i;
    int randomValue = 0;
    //char* returnString = "";
    //char returnString[5];
    for (i = 0; i < num; i++) {
        //we will only need one random number 
        //char* returnString;
        randomValue = (rand() % (upperValue - lowerValue + 1)) + lowerValue;
        //sprintf(returnString, "%d", randomValue);
        //itoa(randomValue, returnString, 10);
        //return returnString;
    }
    
    return randomValue;
}

    //fact Command is our simpler built in that outputs an interesting fact for a specified or random category
    static void factCommand(char* index, int intIndex) {

        //In parse pipeline, we are going to call factCommand(command->argv[1], -1)
        //If it's random, then we are going to call factCommand("redo", randomized int)
        
        if (intIndex == 0 || strcmp(index, "animals") == 0) {
            printf("Interesting Animal Fact: A crocodile cannot stick it's tongue out\n");

        }
        else if (intIndex == 1 || strcmp(index, "food") == 0) {
            printf("Interesting Food Fact: Strawberries are not actually berries since their seeds are on the outside\n");
        }
        else if (intIndex == 2 || strcmp(index, "computers") == 0) {
            printf("Interesting Computer Fact: The First Computer Weighed More Than 27 Tons\n");
        }
        else if (intIndex == 3 || strcmp(index, "sports") == 0) {
            printf("Interesting Sports Fact: The average life span of an MLB baseball is five to seven pitches\n");
        }
        else if (intIndex == 4 || strcmp(index, "history") == 0) {
            printf("Interesting History Fact: Cleopatra, the last Queen of Egypt, was not Egyptian. She was Greek\n");
        }
        else if (intIndex == 5 || strcmp(index, "movies") == 0) {
            printf("Interesting Movie Fact: Alfred Hitchcock's Psycho was the first U.S. film to feature a toilet flusing\n");
        }
        else if (intIndex == 6 || strcmp(index, "politics") == 0) {
            printf("Interesting Politics Fact: Approximately 15,000 books have been written about Abraham Lincoln\n");
        }
        else {
            //get a ranomd number between 0 and 5, then call the method again
            int newIndex = calculateRandomValue(0, 6, 1);
            factCommand("random", newIndex);
        }

    }


    //showHistoryCommand() is called to print out the command line history
    static void showHistoryCommand() {
        //Functions to use: 
        //  1) void add_history (const char *string)
        //  2) clear_history (void)
        //  3) int history_expand (char *string, char **output)
        //  4) int write_history (const char *filename)
        //  5) HIST_ENTRY ** history_list (void)

        the_history_list = history_list();

        //int listSize =  sizeof(the_history_list)/sizeof(the_history_list[0]);

        for (int i = 0; i < history_length; i++) {
            printf("%d: %s\n", i, the_history_list[i]->line);
        }

        //print all of the elements in the the_history_list

    }

    //clearHistoryCommand() is called to reset the command line history
    static void clearHistoryCommand() {
        
        clear_history();
    }


    //With history, call the history command, then check that the history command was in the output

    //---------------- End Custom Job functions ------------------------
    
    //jobCommand is called to print out all active jobs in our job_list
    static void jobCommand(struct ast_command *command) {
        //Loop through job_list and print each one out with print_job
        
        for (struct list_elem * e = list_begin(&job_list); 
         e != list_end(&job_list); 
         e = list_next(e)) {
        
        struct job *currJob = list_entry(e, struct job, elem);
            if (currJob->num_processes_alive > 0) {
                
                print_job(currJob);  //should the input be currJob ??
            }
        
        }
    }

    //killCommand is called to terminate a specific job
    //We first have to check if the command the user is looking
    //for exists in our job_list then sending the right signal
    //to terminate the command
    static int killCommand(struct ast_command *command) {

        if (command->argv[1] == NULL) {
            //print out kill: job id missing
            fprintf(stderr, "kill: job id missing\n");
        }
        else {
            //retrieve the job from argv[1]
            //static struct job * get_job_from_jid(int jid)
            char* endOfArray;
            int jobID = strtol(command->argv[1], &endOfArray, 20);
            if (command->argv[1] == endOfArray) {
                fprintf(stderr, "kill: usage kill <job>/n");
                return 0;
            }
            struct job *currentJob = get_job_from_jid(jobID);
            
            if (currentJob == NULL) {
                //invalid job jid
                printf("%s %s: No such job\n", command->argv[0], command->argv[1]);
            }
            else {
                //currentJob is valid
                
                int processID = currentJob->processGroupID;
                //send a signal to the child to tell it to stop executing process
                //SIGTERM
                killpg(processID, SIGTERM);
                //When you get to pipes, you have to figure out how to put multople processes 
                //set_process_groupid to set all of the process groups to the same id
            }
        }
        return 0;
    }

    //The fgCommand command is called when the user wants to 
    //move a process into the foreground. We first need to check
    //if the job they are looking for exists in our job_list, then
    //saving the terminal state and sending the appropriate signal
    //depending on if the process is running in the background or stopped
    static void fgCommand(struct ast_command *command) {

        //check if the job id is valid
        if (command->argv[1] == NULL) {
            //print out fg: job id misssing
            fprintf(stderr, "fg: job id missing\n");
        }
        else {
            struct job *currJob = get_job_from_jid(atoi(command->argv[1])); //jid or pid
            if (currJob == NULL) {
                fprintf(stderr, "%s %s: no such job\n", command->argv[0], command->argv[1]);
            }
            else {
                //job is valid
                //same stopped case for the bg command
                //send special signal for jobs that have NEEDSTERMINAL status

                //make sure you are restoring terminal state
                //one of the term state functions in term
                //termstate_save with parameter saved_tty_state
                //saving happens when a process gets stopped
                //at that point save the terminal 
                //terminal state gets saved in foreground
                //terminal gets saved in ctrl z or stop

                //const char* jobStatus = get_status(currJob->status);
                if (currJob->status == STOPPED || currJob->status == NEEDSTERMINAL) {

                    //make sure you are printing the currJob name before executing it
                    print_cmdline(currJob->pipe);
                    printf("\n");
                    signal_block(SIGCHLD);
                    //termstate_give_terminal_to(&currJob->saved_tty_state, currJob->processGroupID);
                    termstate_give_terminal_to(NULL, currJob->processGroupID);
                    killpg(currJob->processGroupID, SIGCONT);
                    currJob->status = FOREGROUND;
                    wait_for_job(currJob);
                    
                    termstate_give_terminal_back_to_shell();
                    signal_unblock(SIGCHLD);
                }
                else {
                    //fprintf(stderr, "%s %s\n", command->argv[0], command->argv[1]);
                    //printf("background job\n");
                    //printf("%s", "");
                    print_cmdline(currJob->pipe);
                    printf("\n");
                    termstate_give_terminal_to(NULL, currJob->processGroupID);
                    //we are not calling SIGCONT
                    currJob->status = FOREGROUND;
                    wait_for_job(currJob);
                    termstate_give_terminal_back_to_shell();
                }


                //two cases: 1) when the child is already running in the background
                        //1) cush doesnt have anything in the foreground so just have the system wait for the background process already running
                //2) When you've stopped the process. Process is stopped but still in the job
                        //send a signal to the process group to revive job to turn it back into a running job

                //check if there is already a job in the foreground
                //go through each job and check if any return "FOREGROUND" from get_status
                //there is no way we would be able to call fg if there were a process running in the foreground 
                //so do we not have to worry about a job already being in the foreground 

            }

        }

    }

    //static void bgCommand(struct ast_command *command, struct job *job) {
    static void bgCommand(struct ast_command *command) {
        if (command->argv[1] == NULL) {
            //print out bg: job id misssing
            fprintf(stderr, "bg: job id missing\n");
        }
        else {
            struct job *currJob = get_job_from_jid(atoi(command->argv[1])); //jid or pid
            if (currJob == NULL) {
                //A command with the corresponding jid could not be found in the job_list
                fprintf(stderr, "%s %s: no such job", command->argv[0], command->argv[1]);
            }
            else {
                //job is valid 
                //Same stuff without having to wait for it
                //bg command cannot be called when a command has status Foreground
                const char* jobStatus = get_status(currJob->status);

                if (strcmp(jobStatus, "Stopped") == 0 || strcmp(jobStatus, "Stopped (tty)") == 0) {
                    killpg(currJob->processGroupID, SIGCONT);
                    currJob->status = BACKGROUND;
                    
                }
                else if (strcmp(jobStatus, "Background") == 0) {
                    
                    //current job is already in the background 
                    fprintf(stdout, "%s: %s already in the background", command->argv[0], command->argv[1]);

                }

            }

        }
    }

    static void exitCommand(struct ast_command *command) {
        //call exit(0) to exit
        exit(0);
        
    }


    static void stopCommand(struct ast_command *command) {
        //Similar set up and error checks to bgcommand and fgcommand
        if (command->argv[1] == NULL) {
            //print out fg: job id misssing
            fprintf(stderr, "stop: job id missing\n");
        }
        else {
            struct job *currJob = get_job_from_jid(atoi(command->argv[1]));
            if (currJob == NULL) {
                fprintf(stderr, "%s %s: no such job\n", command->argv[0], command->argv[1]);
            }
            else {
                //job is valid 
                //if you call stop with a running job, there is no output
                const char* jobStatus = get_status(currJob->status);

                if (strcmp(jobStatus, "Stopped") == 0) {
                    
                    //The job has already been stopped and prints out the job details
                    print_job(currJob);
                }
                else {
                    //the process needs to be stopped and is currently running or needs access to terminal
                    //Calling killpg with the right signal may be right to stop a process
                    
                    killpg(currJob->processGroupID, SIGSTOP); //Not sure about the signal value
                    currJob->status = STOPPED;

                }
                
            }

        }

    }

    static void updateJobList() {
        //We only want jobs that are alive/active in our job list
        //Call this whenever text is being entered.
        //Like before sleep 10 is before being run
        //Before you start running the command itself

        //Loop to go through job list
        for (struct list_elem * e = list_begin(&job_list); 
         e != list_end(&job_list); 
         ) {

             //delete jobs if the num_processes_alive is 0
            struct job *currJob = list_entry(e, struct job, elem);

            if (currJob->num_processes_alive == 0) {
                e = list_remove(e);
                delete_job(currJob);
            }
            else {
                e = list_next(e);
            }

             
         }

    }


    //We want to write our own handler for ctrl c and ctrl z
    //signal_set_handler(SIGCHLD, sigchld_handler);
    //instead of terminating 
    //copy implementation of built ins into the ctrl c and ctrl z
    //We are going to be calling the set_signal_handler

    extern char **environ;

    static void parse_pipelines(struct ast_command_line *cmdline) {
        //Pipes are arrays of two values 
        //two pipes: before and after pipe
        
        int beforePipe[2];
        int afterPipe[2];
        beforePipe[0] = 0;

        //we only need to copy the 1
        //afterPipe[1] gets closed
        //beforePipe[0] 

        //If it's the first process, connect the after pip
            //afterPipe = pipe();
            //then connect to the write end of the after pipe then close the write, only thing we have is afterpipe[0], which gets copied to beforePipe[0]
            //beforePipe[0] = afterPipe[0], basically get the next pair of the pipes

        //second process (middle process)
            //  connect to beforePipe[0] for inputPipe[0], close beforePipe[0], posix_spawnpdup2(beforePipe[0], stdin)
            // for afterPipe = create new pipe pipe(); in the parent
            // connect afterPipe[1] (dup2(write end of afterPipe, stdout))
            // close afterPipe[1] **only close after psoix_spawnp
            // beforePipe[0] = afterPipe[0]
            // If you have an afterPipe, if you are not the last process, do what's above up from 946

        //last process
            //dup2(beforePipe[0], stdin)
            //close beforePipe[0]

        //the only thing you want to do in the parent are pipe and close
        //everything in the child should be posix_spawn

        //two components: pipes between components and output
        //cat < input.txt - read from input.txt and pipe that into echo print
        //connect the processes with a one directional pipe
        //Also file input and output. First process in the pipe should be connected
        //If there is a file, then there will be a string ioinput
        //posix-spawn_open to open input or output file
        
        //we are going to take the pipe file descriptor and pipe it into stdin or stdout using dup2
        //there is a thing to append stdin to stdout, call adddup2 right before posix_spawn

        for (struct list_elem * currPipeline = list_begin (&cmdline->pipes);
            //This loop goes through each ast_pipeline in the ast_Command_line
            currPipeline != list_end (&cmdline->pipes); 
            currPipeline = list_next (currPipeline)) {
            struct ast_pipeline *pipe = list_entry(currPipeline, struct ast_pipeline, elem);

            bool isBuiltIn = false;

            struct job *job;

            signal_block(SIGCHLD);

            //-------------- First Command from pipe->commands check ---------------------------------------
            //check the first command, if it's a built in then call the function as a built in ********
            
            struct ast_command *firstCommand = list_entry(list_begin(&pipe->commands), struct ast_command, elem);

            if (strcmp(firstCommand->argv[0], "jobs") == 0 || strcmp(firstCommand->argv[0], "fg") == 0 || strcmp(firstCommand->argv[0], "bg") == 0 || strcmp(firstCommand->argv[0], "kill") == 0 || strcmp(firstCommand->argv[0], "exit") == 0 || strcmp(firstCommand->argv[0], "stop") == 0 || strcmp(firstCommand->argv[0], "fact") == 0 || strcmp(firstCommand->argv[0], "history") == 0 || strcmp(firstCommand->argv[0], "clear_history") == 0) {
                isBuiltIn = true;
                        
                        if (strcmp(firstCommand->argv[0], "jobs") == 0) {
                            jobCommand(firstCommand); //manage a list of jobs **get_job_from_jid
                        }
                        else if (strcmp(firstCommand->argv[0], "fg") == 0) {
                            fgCommand(firstCommand); //brings a job to the foreground
                        }
                        else if (strcmp(firstCommand->argv[0], "bg") == 0) {
                            bgCommand(firstCommand); //brings a job to the background
                        }
                        else if (strcmp(firstCommand->argv[0], "kill") == 0) {
                            killCommand(firstCommand); //kill the specified job
                        }
                        else if (strcmp(firstCommand->argv[0], "exit") == 0) {
                            exitCommand(firstCommand); //exit cush by calling exit(0)
                        }
                        else if (strcmp(firstCommand->argv[0], "stop") == 0) {
                            stopCommand(firstCommand); //Send a signal to stop the specified job
                        }
                        else if (strcmp(firstCommand->argv[0], "fact") == 0) {
                            //our first custom built in
                            factCommand(firstCommand->argv[1], -1);
                        }
                        else if (strcmp(firstCommand->argv[0], "history") == 0) {
                            //display command line history
                            showHistoryCommand();
                        }
                        else if (strcmp(firstCommand->argv[0], "clear_history") == 0) {
                            clear_history(); //clear command line history
                        }
                    
            }
            else {


            
            job = add_job(pipe); //one job per pipeline, only for none built ins
            //signal_unblock(SIGCHLD);


            //Piping signal check
                //if iored_input is not NULL, first command should read from 'iored_input'
                //if iored_output is not NULL, last command should write to 'iored_output'
                //pipe, pipe2 - create pipe that can be used for interprocess communication
                //pipe2(int pipefd[2], int flags)

            //For Ctrl-Z, you do not need to implement it directly if your foreground is implemented correctly

            //if processGroupID is not set, set it equal to the pid of the first child 
        

            //if a job every leaves the foreground, then you need to save the terminal state

            for (struct list_elem * currCommand = list_begin(&pipe->commands);
                currCommand != list_end (&pipe->commands); 
                currCommand = list_next (currCommand)) {
                struct ast_command *command = list_entry(currCommand, struct ast_command, elem);


                        //we are going to be modifying
                        //posix spawn file actions init
                        //posix spawn file actions adddup2
                        //posix spawn file actions addclose


                        pid_t child_pid;
                        int s;
                        
                        posix_spawnattr_t attrp;
                        posix_spawn_file_actions_t file_actionsp;
                        posix_spawnattr_init(&attrp);
                        posix_spawn_file_actions_init(&file_actionsp);
                        

                        //addopen is converting STDIN_FILENO to pipe->iored_input
                        //STDOUT_FILENO for iored_output

                        

                        //At each process we hold 2 pipes, the pipe before the process and after the process
                        //Iterate over all the process. If you are not the left most child, then you do connect the left pipe
                        //If you are not the last child, connect to the after pipe
                        //use addup2 and addclose
                        //Make you are closing all the file descriptors in the children. And also in the parent
                        //In the children you will be doing the posix-spawn file actions, addup2 and addclose
                        //In the parent, you are just going to be using close
                        //Make sure to do close only after posix_spawnp
                        //Before and after pipe. After you are done, you are going to shift them over by one and put the before pipe into the after pipe
                        
                       
                        // ----------------- Setting up Pipes --------------------------

                        //if append_to_output is true, make a posix_spawn_file_actions_addopen call with different signals

                        bool firstProcess = false;
                        bool middleProcess = false;
                        bool lastProcess = false;


                        //Middle process ****
                        if (list_begin(&pipe->commands) != currCommand && list_rbegin(&pipe->commands) != currCommand) {
                            middleProcess = true;
                            //posix_spawnpdup2(beforePipe[0], stdin);
                            posix_spawn_file_actions_adddup2(&file_actionsp, beforePipe[0], STDIN_FILENO);
                            pipe2(afterPipe, O_CLOEXEC); //afterPipe = pipe();
                            posix_spawn_file_actions_adddup2(&file_actionsp, afterPipe[1], STDOUT_FILENO); //(dup2(write end of afterPipe, stdout))
                            //posix_spawn_dup2(afterPipe[1], stdout);
                            //close afterPipe[1] after posix_spawnp ***
                            
                        }

                        //First Process ****
                        if (list_begin(&pipe->commands) == currCommand) {

                            if (pipe->iored_input != NULL) {
                                posix_spawn_file_actions_addopen(&file_actionsp, STDIN_FILENO, pipe->iored_input, O_RDONLY, S_IRWXO);
                            }
                            
                            if (list_rbegin(&pipe->commands) != currCommand) {
                            firstProcess = true;
                            pipe2(afterPipe, O_CLOEXEC); //initalizes the afterPipe, output of the first command 
                            posix_spawn_file_actions_adddup2(&file_actionsp, afterPipe[1], STDOUT_FILENO);
                            }
                            
                        }

                        //Last Process ****
                        if (list_rbegin(&pipe->commands) == currCommand) {
                            
                            if (pipe->iored_output != NULL) {
                                //Add if statement for append_to_output

                                if (pipe->append_to_output) { //*** need to update
                                    
                                    //Access Modes: O_RDONLY, O_WRONLY, or O_RDWR
                                    //change flag parameters if we have to append to the end of an output file
                                    posix_spawn_file_actions_addopen(&file_actionsp, STDOUT_FILENO, pipe->iored_output, O_WRONLY | O_CREAT | O_APPEND, S_IRWXU);
                                }
                                else {
                                //posix_spawn_file_actions_addopen(&file_actionsp, STDOUT_FILENO, pipe->iored_input, O_RDWR, S_IRWXO);
                                posix_spawn_file_actions_addopen(&file_actionsp, STDOUT_FILENO, pipe->iored_output, O_WRONLY | O_CREAT, S_IRWXU);
                                }

                            }

                            if (list_begin(&pipe->commands) != currCommand) {
                                lastProcess = true;
                                posix_spawn_file_actions_adddup2(&file_actionsp, beforePipe[0], STDIN_FILENO);
                            }
                            
                            
                            
                        }

                        
                        //--------------- End of Setting up Pipes ----------------

                        //input, output
                        


                        /* Spawn the child. The name of the program to execute and the
                            command-line arguments are taken from the command-line arguments
                            of this program. The environment of the program execed in the
                            child is made the same as the parent's environment. */
                            //pid_t child_pid;
                        //s = posix_spawnp(&child_pid, command->argv[0], file_actionsp, attrp,
                        //    &argv[optind], environ);

                        //if it is a foregoround process, add tcsetgroup
                        //two cases 
                        //you would only 
                        //we always call setpgroup with either 0 or the child_pid from posix_spawnp

                        int flags = POSIX_SPAWN_SETPGROUP;

                        //bool foregroundProcess = false;
                        //bool firstProcess = false;

                        if (job->status == FOREGROUND) {
                            flags = flags | POSIX_SPAWN_TCSETPGROUP;
                            posix_spawnattr_tcsetpgrp_np(&attrp, termstate_get_tty_fd());
                        }
                        
                        posix_spawnattr_setflags(&attrp, flags);
                        posix_spawnattr_setpgroup(&attrp, job->processGroupID);


                        //For every process we need to call init, setflags and setpgroup but 
                            //with 2 flags if it's a foreground process and 
                            //setpgroup with 0 if firstProcess = true or child_pid if not


                        //Get posix_spawnp to/ run with ls first
                        //For the last one get the environment variable to pass
                        //Pass the environment 
                        //Get the environment variable then pass it in to each process
                        //pid_t child_pid;

                        //By the time you call posix_spawnp the child process is already set. Everything should happen before posix_spawnp
                        //if it's not the first process, connect the after pipe to the next process' before pipe
                        //At some point you should be swapping before = after and after = new pipe
                        //make sure in the parent process to close pipe with close()
                        //Whenever you need to use an after pipe you are going to create a new pipe
                        //Whenever you write data for n processes you should be

                        if (command->dup_stderr_to_stdout) {
                            posix_spawn_file_actions_adddup2(&file_actionsp, STDOUT_FILENO, STDERR_FILENO);
                        }

                        
                       s = posix_spawnp(&child_pid, command->argv[0], &file_actionsp, &attrp, command->argv, environ);
                
                        if (s != 0) {
                            fprintf(stderr, "%s: no such file or directory\n", command->argv[0]);
                            continue;
                        }
                        job->num_processes_alive++;

                        //Part of the middle process execution section
                        if (middleProcess) {
                            close(afterPipe[1]);
                            close(beforePipe[0]);
                            beforePipe[0] = afterPipe[0];
                        }
                        else if (firstProcess) {
                            close(afterPipe[1]);
                            beforePipe[0] = afterPipe[0];
                        }
                        else if (lastProcess) {
                            close(beforePipe[0]);
                        }
                        
                        if (job->processGroupID == 0) { //First job in the pipeline
                            job->processGroupID = child_pid; //1 process per pipeline so we only set processGroupID once
                            
                        }

                        struct pid *currPid = malloc(sizeof(struct pid));
                        currPid->pidValue = child_pid;

                        list_push_back(&job->pidList, &currPid->elem);

                        //Initalizing values with posix_spawn calls 
                        //For each process we need to:
                            //1) Call posix_spawnattr_setflags
                                //if forgroundProcess == true, call with POSIX_SPAWN_SETPGROUP and TCSETGROUP
                            //2) Call  posix_spawnattr_setpgroup
                                //if firstProcess == true, call with 0, otherwise call with child_pid
                        //we want to append to the list of flags then 


                        //int firstChildPID;
                            //store pid of first child

                       if (pipe->bg_job) {
                           fprintf(stderr, "[%d] %d\n", job->jid, job->processGroupID);
                       }

                        
                        
                    }

            }
         
         //call wait for jobs here
         //You do not want to wait for background jobs
         //only wait when the job_status of job is foreground

         //if the signal handler already changed the job state, then the job status may have already changed


            //we want to do a check to see if we should wait for a non-built
            //only want to wait for foreground jobs *****
            if (isBuiltIn == false && !pipe->bg_job) { //AND is a background job
                wait_for_job(job);
            }

            signal_unblock(SIGCHLD); //part of wait for job. It's blocking itself until it gets that signal
        }
        
    }



int
main(int ac, char *av[])
{
    int opt;
    using_history();

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    //signal(SIGTSTP, SIG_IGN);

    //get the signal mask of the parent, then pass it to posix_spawnp

    //get the parent signal mask, unignore sigint, sigst
    //call signprocmask before you ignmore ctrl z then pass the mask to the child
    //give it parameters that tell it to not modify anything. omask will be the returned mask
    
    sigprocmask(0, NULL, &omask);

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    signal(SIGTSTP, SIG_IGN);
    
    signal_set_handler(SIGTSTP, sigtstp_handler);
    signal_set_handler(SIGINT, sigint_handler);
    
    termstate_init();

    //sigtstp_handler

    /* Read/eval loop. */
    for (;;) {
        termstate_give_terminal_back_to_shell(); //you only need to call this once
        /* Do not output a prompt unless shell's stdin is a terminal */
        char * prompt = isatty(0) ? build_prompt() : NULL;
        char * cmdline = readline(prompt);
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;

        char *expansion;
        int result;

        result = history_expand (cmdline, &expansion);
        free(cmdline);

        if (result < 0 || result == 2)
        {
                fprintf(stderr, "%s\n", expansion);
              free (expansion);
              continue;
        }

        //char *expansion;
        if (strcmp(expansion, "history") != 0 && strcmp(cmdline, "clear_history") != 0) {
        
        add_history (expansion);
        
        

        //add_history(cmdline);
        //history_write_timestamps = 1;  //timestamps will be written to history entries
        }
        
        struct ast_command_line * cline = ast_parse_command_line(expansion);
        free (expansion);
        
        
        
        //free (cmdline);
        if (cline == NULL)                  /* Error in command line */
            continue;

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }

        //ast_command_line_print(cline); //going to call our public function here
        updateJobList();
        parse_pipelines(cline);
        updateJobList();
        //cmdline is a ast_command_line struct that contains all of the data we need for each line
        free(cline);
        //ast_command_line_free(cline);
    }
    return 0;
}
