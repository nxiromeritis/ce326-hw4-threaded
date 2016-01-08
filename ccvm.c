#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>


// xxd -r -p tmp
#define DEBUG2

#define L_ROOT -4
#define L_END -3
#define D_RDY -2
#define D_BLC -1

#define READY 1
#define BLOCKED 2
#define SLEEPING 3
#define STOPPED 4

// body information stored here
struct body_info {
	int locals_size;
	int code_size;
	int start_of_code;
};
typedef struct body_info body_info_t;


struct tasks {
	int task_body;		// we need to know which body corresponds to each task
	int min_pc;			// min/max pc values are needed to check if a task by..
	int max_pc;			// ..mistake accesses another body

	struct tasks *nxt;
	struct tasks *prv;

	//local task state variables
	int id;
	int state;
	char reg[16];
	int pc;
	int sem;
	int waket;
	char *local_mem;
};


// information needed by pthreads (could be in a struct and passed as thread argument)
int workers;				// total workers
int *wid;					// worker id
int *wtasks;				// number of tasks for  each worker
struct tasks *rdy_root;		// root for ready task list (one root for every worker)
struct tasks *blc_root;		// root for blocked task list (one root for every worker)

pthread_mutex_t cs_mtx;		// used in instruction cs
pthread_mutex_t main_mtx;	// used to prevent main from terminating before workers
pthread_mutex_t *wrk_mtx;	// one for each worker to wait when all his tasks are blocked

int num_of_tasks;			// number of tasks
int num_of_blocked = 0;		// number of blocked tasks
int num_of_stopped = 0;		// tasks that are done with execution

char *globalMem;
char *code;
struct tasks **cur;			// holds the node address of the current running task for every worker

body_info_t *bd_info;

// list functions
void list_init();
void list_insert(int id, int task_body, int task_arg, int worker);		//insert after root
int  list_delete(struct tasks *node, int worker);					// remove node from rdy list
int  list_move_to(struct tasks *node, int dst, int worker);			// move a node between lists of multiple workers
void list_locate_move_blocked(int addr);							// locate blocked task at an addr
void print_lists();
void print_node();


// .bin related functions
void read_bytes(int fd,	char *data, int bytes);
void trace(char *str, int len);		//only for debugging
void load_bin(int fd);
void *run_bin(void *wid);


int main(int argc, char *argv[]) {
	int fd_bin;
	int worker;
	pthread_t *pworkers;

	workers = 1;	// default value, probably will be changed by argv[3]

	if ((argc != 2)&&(argc != 3)) {
		printf("Invalid number of arguments\nExiting..\n");
		return(0);
	}
	else {
		fd_bin = open(argv[1], O_RDONLY);
		if (fd_bin == -1) {
			if (errno == ENOENT) {
				printf("No such file in current directory.\nExiting..\n");
				return(0);
			}
			else {
				perror("open");
				exit(1);
			}
		}
		if (argc == 3) {
			workers = atoi(argv[2]);
			if (workers <= 0) {
				printf("Error: Invalid argument(3).\nExiting..\n");
				exit(1);
			}
			else {
				printf("Number of workers: %d\n", workers);
			}
		}
	}

	// allocate memory for array which stores number of tasks for each worker
	wtasks = (int *)malloc(sizeof(int)*workers);
	if (wtasks == NULL) {
		printf("Error: Memory allocation problems.\nExiting..\n");
		exit(1);
	}

	// allocate memory for every worker thread
	pworkers = (pthread_t *)malloc(sizeof(pthread_t)*workers);
	if (pworkers == NULL) {
		printf("Error: Memory allocation problems.\nExiting..\n");
		exit(1);
	}

	// allocate memory for every worker mutex
	wrk_mtx = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t)*workers);
	if (wrk_mtx == NULL) {
		printf("Error: Memory allocation problems.\nExiting..\n");
		exit(1);
	}

	// allocate memory for worker id array (used only in thread creation)
	wid = (int *)malloc(sizeof(int)*workers);
	if (wid == NULL) {
		printf("Error: Memory allocation problems.\nExiting..\n");
		exit(1);
	}

	// read file and store nessecary information
	load_bin(fd_bin);
	if (close(fd_bin)) {
		perror("close");
		exit(1);
	}

	// initialize cs_mtx to 1
	if (pthread_mutex_init(&cs_mtx, NULL)) {
		perror("pthread_mutex_init");
		exit(1);
	}

	// initialize main_mtx to 0
	if (pthread_mutex_init(&main_mtx, NULL)) {
		perror("prhead_mutex_init");
		exit(1);
	}
	if (pthread_mutex_lock(&main_mtx)) {
		perror("pthread_mutex_lock");
		exit(1);
	}

	// for every worker
	for(worker=0; worker<workers; worker++) {

		// initialize mutex to 0
		if(pthread_mutex_init(&wrk_mtx[worker], NULL)) {
			perror("pthread_mutex_init");
			exit(1);
		}
		if(pthread_mutex_lock(&wrk_mtx[worker])) {
			perror("pthread_mutex_lock");
			exit(1);
		}

		// create thread
		wid[worker] = worker;
		if (pthread_create(&pworkers[worker], NULL, run_bin, (void *)&wid[worker])) {
			perror("pthread_create");
			exit(1);
		}
	}

	if (pthread_mutex_lock(&main_mtx)) {
		perror("pthread_mutex_lock");
		exit(1);
	}

	printf("\nBye from main.\n");
	return(0);
}


// initialize lists and cur(==current running task) for each worker
void list_init() {
	int i;

	// roots of lists containing READY,SLEEPING tasks
	rdy_root = (struct tasks *)malloc(workers*sizeof(struct tasks));
	if (rdy_root == NULL) {
		printf("Memory allocation problems.\nExiting..\n");
		exit(1);
	}

	// roots of lists containing BLOCKED tasks
	blc_root = (struct tasks *)malloc(workers*sizeof(struct tasks));
	if (blc_root == NULL) {
		printf("Memory allocation problems.\nExiting..\n");
		exit(1);
	}

	// current running task (for each worker thread)
	cur = (struct tasks **)malloc(workers*sizeof(struct tasks *));
	if (cur == NULL) {
		printf("Memory allocation problams.\nExiting..\n");
		exit(1);
	}

	// make empty circular lists
	for (i=0; i<workers; i++) {
		rdy_root[i].nxt = &rdy_root[i];
		rdy_root[i].prv = &rdy_root[i];
		rdy_root[i].id = -1;

		blc_root[i].nxt = &blc_root[i];
		blc_root[i].prv = &blc_root[i];
		blc_root[i].id = -1;
	}
}


// insert to ready(state) worker's list and initialize node
void list_insert(int id, int task_body, int task_arg, int worker) {
	struct tasks *curr;

	curr = (struct tasks *)malloc(sizeof(struct tasks));
	if (curr == NULL) {
		printf("Memory allocation problems.\nExiting..\n");
		exit(1);
	}

	// initialize node fields
	curr->id = id;
	curr->state = READY;
	curr->task_body = task_body;
	curr->min_pc = bd_info[task_body-1].start_of_code;
	curr->max_pc = curr->min_pc + bd_info[task_body-1].code_size - 1;
	curr->pc = bd_info[task_body-1].start_of_code;

	curr->local_mem = (char *)malloc(bd_info[task_body-1].locals_size*sizeof(char));
	if(curr->local_mem == NULL) {
		printf("Memory allocation problems.\nExiting..\n");
		exit(1);
	}

	// put task argument to last local memory segment
	curr->local_mem[bd_info[task_body-1].locals_size-1] = task_arg;

	curr->sem = -1;
	curr->waket = -1;

	curr->nxt = rdy_root[worker].nxt;
	curr->prv = &rdy_root[worker];
	curr->nxt->prv = curr;
	curr->prv->nxt = curr;

	print_node(curr);
	print_lists();
}



// we can delete a node only if it belongs to rdy_list
int list_delete(struct tasks *node, int worker) {
	struct tasks *curr;

	// id's are unique. check if node exists in rdy_list
	for(curr=rdy_root[worker].nxt; (curr->id != node->id)&&(curr->id != -1); curr=curr->nxt);
	if (curr->id == -1) {
		printf("list_remove:\nError: Node does not exist in list or has root id\n");
		return(1);
	}

	if (curr->state != STOPPED) {
		printf("list_delete: Task terminated unexpectedly (state != STOPPED)..\n");
	}

	node->nxt->prv = node->prv;
	node->prv->nxt = node->nxt;
	free(node->local_mem);
	free(node);

	return(0);
}


// called only when a task must go blocked or ready
// move node with id from D_RDY/D_BLC to D_BLC/D_RDY(=dst)
// if dst == D_BLC then node will be added at the end (before root)
// if dst == D_RDY then node will be added before id of current running task(global cur)
int list_move_to(struct tasks *node, int dst, int worker) {
	struct tasks *root2;	// root of the list where the node will be finally inserted
	struct tasks *curr2;	// argument node will be inserted after curr2

	if (dst == D_RDY) {
		root2 = &rdy_root[worker];
		// in this case we want one task (node) to go from blocked to ready
		// this cant be cur because this is the one that called UP
		if (node->id == cur[worker]->id) {
			printf("list_move_to:\nError: node and curr_addr are the same\n");
			return(1);
		}
		curr2 = cur[worker]->prv;		// set destination node
	}
	else if (dst == D_BLC) {
		root2 = &blc_root[worker];
		curr2 = root2->prv;		// set destination node
	}
	else {
		printf("list_move_to:\nError: Unknown list reference (dst).\n");
	}

	// detach node from list
	node->nxt->prv = node->prv;
	node->prv->nxt = node->nxt;

	//insert node to new list and position
	node->nxt = curr2->nxt;
	node->prv = curr2;
	node->nxt->prv = node;
	node->prv->nxt = node;


	if ((dst == D_RDY)&&(node->nxt->nxt == node)) {
		/*printf("I unblocked you\n");*/
		if(pthread_mutex_unlock(&wrk_mtx[worker])) {
			perror("pthread_mutex_unlock");
			exit(1);
		}
	}

	return(0);
}


// called only when task must be unblocked from a semaphore
// when this function is called it is GUARANTEED that it will eventually find the blocked node
void list_locate_move_blocked(int addr) {
	struct tasks *curr;
	int worker;

	// this while is needed in a very rare case where there is ONLY one blocked task
	// and has not been added to the list yet
	while(1) {
		worker = 0;
		while(worker<workers) {
			for (curr=blc_root[worker].nxt; (curr->sem!=addr)&&(curr->id!=-1); curr=curr->nxt);
			if (curr->id != -1) {break;}	// blocked node
			worker++;
		}
		if (curr->id == -1) {
			printf("Error: Could not locate blocked node at given addr\nRe-trying");
		}
		else {
			/*printf("found blocked task\n");*/
			break;
		}
	}

	// after our node has been located move it to the corresponding rdy_list
	curr->state = READY;
	curr->sem = -1;
	list_move_to(curr, D_RDY, worker);
}

// used for debugging
void print_lists() {
#ifdef DEBUG
	struct tasks *curr;
	int i;

	for (i=0; i<workers; i++) {
		printf("Worker %d:\n", i);
		printf("\tNonBlocked:");
		for (curr=rdy_root[i].nxt; (curr->id!=-1); curr=curr->nxt){
			printf(" %d->", curr->id);
		}
		printf("\n");

		printf("\tBlocked:");
		for (curr=blc_root[i].nxt; (curr->id!=-1); curr=curr->nxt) {
			printf(" %d->", curr->id);
		}
		printf("\n");
	}

#endif
}

// used for debugging
void print_node(struct tasks *node) {
#ifdef DEBUG
	printf("\nNew Task:\n");
	printf("ID: \t%d\n", node->id);
	printf("STATE: \t%d\n", node->state);
	printf("PC: \t%d\n", node->pc);
	printf("SEM: \t%d\n", node->sem);
	printf("WAKET: \t%d\n", node->waket);
	printf("ARG: \t%d\n", node->local_mem[bd_info[node->task_body-1].locals_size-1]);
	printf("TSKBD: \t%d\n", node->task_body);
	printf("MINPC: \t%d\n", node->min_pc);
	printf("MAXPC: \t%d\n", node->max_pc);
	printf("\n");
#endif
}

// used for debugging (displaying .bin contents)
void trace(char *str, int len) {
#ifdef DEBUG
	int i;

	/*len = (int)strlen(str);*/
	/*printf("len = %d\n", len);*/
	for(i=0; i<len; i++) {
		printf("%02x ", str[i] & 0xff);
	}
	printf("\n");
#endif
}



// reads the specified amount of bytes from the bin file
// from the point where previous read stopped
void read_bytes(int fd, char *data, int bytes) {
	if (read(fd, data, (size_t)bytes) == -1) {
		perror("read");
		exit(1);
	}
	trace(data, bytes);
}



// reads the bin file and stores all nessecary information
void load_bin(int fd) {
	char magic_beg[5] = { 0xde, 0xad, 0xbe, 0xaf, 0x00};
	char magic_bod[5] = { 0xde, 0xad, 0xc0, 0xde, 0x00};
	char magic_tsk[5] = { 0xde, 0xad, 0xba, 0xbe, 0x00};
	char magic_end[5] = { 0xfe, 0xe1, 0xde, 0xad, 0x00};

	char data[5];
	int i;
	int worker;					// used to iterate through workers
	int id;						// used to assign unique ids to tasks
	unsigned int code_index;	// the point from where we should continue writing data to 'code'

	// we could avoid having that many variables but it is prefered that way
	// for better code understanding
	int global_size;
	int num_of_bodies;
	int tot_code_size;
	//int num_of_tasks; //became global
	int code_size;
	int locals_size;
	int task_body;
	int task_arg;

	// used to 'even' the tasks between the workers
	int remaining_jobs;

	printf("Loading file..\n");

	printf("\nHeader Section: \n");

	// read MagicBeg
	read_bytes(fd, data, 4);
	data[4] = '\0';
	if (!strcmp(data, magic_beg)) { printf("\tMagicBeg: MATCHES\n"); }
	else { printf("\tMagicBeg: MISSMATCH\nTerminating..\n"); exit(1);}


	// read global memory size
	read_bytes(fd, data, 1);
	global_size = (unsigned char)data[0];
	printf("\tGlobalSize: %d\n", global_size);

	// allocate memory for global memory
	globalMem = (char *)malloc(global_size*sizeof(char));
	if (globalMem == NULL) {
		printf("Memory allocation problems.\nExiting..\n");
		exit(1);
	}


	// read number of bodies
	read_bytes(fd, data, 1);
	num_of_bodies = (unsigned char)data[0];
	printf("\tNumOfBodies: %d\n", num_of_bodies);

	// allocate memory for body info struct
	bd_info = (body_info_t *)malloc(num_of_bodies*sizeof(body_info_t));
	if(bd_info == NULL) {
		printf("Memory allocation problems.\nExiting..\n");
		exit(1);
	}


	// read total_code_size
	read_bytes(fd, data, 2);
	tot_code_size = (unsigned char)data[0];
	tot_code_size = (tot_code_size << 8) + (unsigned char)data[1];
	printf("\tTotCodeSize: %d\n", tot_code_size);

	// read number of tasks
	read_bytes(fd, data, 1);
	num_of_tasks = (unsigned char)data[0];
	printf("\tNumOfTasks: %d\n\n", num_of_tasks);


	// initialize global memory
	printf("GlobalInit Section: \n");
	read_bytes(fd, (char *)globalMem, global_size);
	printf("(Stored %d bytes)\n\n", global_size);


	// allocate memory for code storage
	code = (char *)malloc(tot_code_size*sizeof(char));
	if(code == NULL) {
		printf("Memory allocation problems.\nExiting..\n");
		exit(1);
	}
	code_index = 0;


	// store code and info from body sections
	for(i=0; i<num_of_bodies; i++) {

		printf("Body(%d) Section: \n", (i+1));

		//read MagicBody
		read_bytes(fd, data, 4);
		data[4] = '\0';
		if (!strcmp(data, magic_bod)) { printf("\tMagicBody: MATCHES\n"); }
		else { printf("\tMagicBody: MISSMATCH\nTerminating..\n"); exit(1);}

		// read current body's total local memory size
		read_bytes(fd, data, 1);
		locals_size = (unsigned char)data[0];
		printf("\tLocalsSize: %d\n", locals_size);

		// read body's code size
		read_bytes(fd, data, 1);
		code_size = (unsigned char)data[0];
		printf("\tCodeSize: %d\n", code_size);

		// store useful body information for later
		bd_info[i].locals_size = locals_size;
		bd_info[i].code_size = code_size;
		bd_info[i].start_of_code = code_index;
		read_bytes(fd, (char *)&code[code_index], code_size);

		// append variable "code" with new body's code
		code_index += code_size;
		printf("---\nlocals_size = %d\n", bd_info[i].locals_size);
		printf("code_size = %d\n", bd_info[i].code_size);
		printf("start_of_code = %d\n", bd_info[i].start_of_code);
		trace((char *)code, code_index);
		printf("\n");
	}
	/*printf("(Total bodies: %d )\n", i);*/


	// taking rid of extra workers (if any)
	if (workers > num_of_tasks) {
		printf("Note: workers(%d) > tasks(%d)\n", workers, num_of_tasks);
		printf("Switching to %d workers.\n", num_of_tasks);
		workers = num_of_tasks;
	}

	// initialize task lists for every worker
	list_init();


	// give each worker the same ammount of tasks
	printf("---\nDetermining No of tasks to be given to each worker\n");
	for(i=0; i<workers; i++) {
		wtasks[i] = num_of_tasks/workers;
	}


	// give remaining tasks(<workers) to some workers
	remaining_jobs = num_of_tasks % workers ;

	printf("%d workers get %d tasks.\n", workers-remaining_jobs, num_of_tasks/workers);
	if (remaining_jobs)
		printf("%d workers get %d tasks.\n", remaining_jobs, (num_of_tasks/workers)+1);
	printf("---\n\n");

	while(remaining_jobs != 0) {
		wtasks[remaining_jobs]++;
		remaining_jobs--;
	}


	id = 0;
	// for each worker
	for (worker=0; worker<workers; worker++) {
		printf("(worker %d)\n", worker);

		// insert each task (with a unique id) to worker's list
		for(i=0; i<wtasks[worker]; i++) {

			printf("Task(%d) Section: \n", (id+1));

			// read MagicTask
			read_bytes(fd, data, 4);
			data[4] = '\0';
			if (!strcmp(data, magic_tsk)) { printf("\tMagicTask: MATCHES\n"); }
			else { printf("\tMagicTask: MISSMATCH\nTerminating..\n"); exit(1);}

			// read task body number
			read_bytes(fd, data, 1);
			task_body = (unsigned char)data[0];
			printf("\tTaskBody: %d\n", task_body);

			// read task arg and store it to the end of local memory
			read_bytes(fd, data, 1);
			task_arg = (unsigned char)data[0];
			printf("\tTaskArg: %d\n", task_arg);

			list_insert(id, task_body, task_arg, worker);
			printf("\n");
			id++;
		}
	}

	// read MagicEnd
	printf("Footer Section: \n");
	read_bytes(fd, data, 4);
	data[4] = '\0';
	if (!strcmp(data, magic_end)) { printf("\tMagicEnd: MATCHES\n"); }
	else { printf("\tMagicEnd: MASSMATCH\nTerminating..\n"); exit(1);}

}

void *run_bin(void *arg) {
	int worker;
	int tasks_stopped = 0;
	int i;
	int inum;
	unsigned char ibyte1;
	unsigned char ibyte2;
	unsigned char ibyte3;
	struct tasks *cur_copy;

	worker = *((int *)arg);
	printf("\nStart of execution:\n");

	cur_copy = NULL;
	cur[worker]=&rdy_root[worker];
	srand(time(NULL));

	while(1) {

		// switch to next task
		cur[worker]=cur[worker]->nxt;


		// if previous task was set perform the right action
		if (cur_copy != NULL) {
			if (cur_copy->state == STOPPED) {list_delete(cur_copy, worker);}
			if (cur_copy->state == BLOCKED) {list_move_to(cur_copy, D_BLC, worker);}
			cur_copy = NULL;
		}

		// if root node is reached go to next node
		if (cur[worker]->id==-1) {cur[worker]=cur[worker]->nxt;}

		// if number of total blocked tasks equals number of tasks -> error
		if (num_of_blocked == num_of_tasks) {
			printf("Error: DEADLOCK\n");
			exit(1);
		}

		// if all tasks (if every worker) are stopped -> exit
		if (num_of_stopped == num_of_tasks) {
			printf("All tasks STOPPED (successfuly).\nEnd of program.\n");
			if (pthread_mutex_unlock(&main_mtx)) {
				perror("pthread_mutex_unlock");
				exit(1);
			}
			return NULL;
		}

		// if cur->nxt is STILL root after all the previous actions
		// then the current worker has all his tasks blocked and waits until someone unblocks it
		if (cur[worker]->id == -1) {
			/*printf("i got locked\n");*/
			if(pthread_mutex_lock(&wrk_mtx[worker])) {
				perror("pthread_mutex_lock");
				exit(1);
			}
			/*continue; */
		}

		// switch to another task if cur is sleeping
		// else continue with execution
		if ((cur[worker]->state==SLEEPING)&&(time(NULL) < cur[worker]->waket)) {continue;}
		if ((cur[worker]->state==SLEEPING)&&(time(NULL) >= cur[worker]->waket)) {
			cur[worker]->state = READY;
			cur[worker]->waket = -1;
		}

		#ifdef DEBUG
		print_lists();
		printf("dbg: switch to id: %d\n", cur[worker]->id);
		/*printf("stopped(%d) vs numoftasks(%d)\n", tasks_stopped, num_of_tasks);*/
		#endif

		i = 0;
		inum = rand()%2+1;
		// execute inum instructions
		while (i<inum) {
			ibyte1 = code[cur[worker]->pc];
			ibyte2 = code[cur[worker]->pc+1];
			ibyte3 = code[cur[worker]->pc+2];

			#ifdef DEBUG
			printf("dbg%d: %02x %02x %02x (pc:%d)\n\n",cur[worker]->id,ibyte1&0xff,ibyte2&0xff,ibyte3&0xff, cur[worker]->pc/3);
			#endif

			// execute instruction
			switch (ibyte1) {
				case 0x01: {   // LLOAD
							   cur[worker]->reg[ibyte2] = cur[worker]->local_mem[ibyte3];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x02: {   // LLOADi
							   cur[worker]->reg[ibyte2] = cur[worker]->local_mem[ibyte3 + cur[worker]->reg[0]];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x03: {   // GLOAD
							   if(pthread_mutex_lock(&cs_mtx)) {
								   perror("pthread_mutex_lock");
								   exit(1);
							   }

							   cur[worker]->reg[ibyte2] = globalMem[ibyte3];
							   cur[worker]->pc += 3;

							   if(pthread_mutex_unlock(&cs_mtx)) {
								   perror("pthread_mutex_unlock");
								   exit(1);
							   }
							   break;
						   }
				case 0x04: {   // GLOADi
							   if(pthread_mutex_lock(&cs_mtx)) {
								   perror("pthread_mutex_lock");
								   exit(1);
							   }

							   cur[worker]->reg[ibyte2] = globalMem[ibyte3 + cur[worker]->reg[0]];
							   cur[worker]->pc += 3;

							   if(pthread_mutex_unlock(&cs_mtx)) {
								   perror("ptread_mutex_unlock");
								   exit(1);
							   }

							   break;
						   }
				case 0x05: {   // LSTORE
							   cur[worker]->local_mem[ibyte3] = cur[worker]->reg[ibyte2];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x06: {   // LSTOREi
							   cur[worker]->local_mem[ibyte3 + cur[worker]->reg[0]] = cur[worker]->reg[ibyte2];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x07: {   // GSTORE
							   if(pthread_mutex_lock(&cs_mtx)) {
								   perror("pthread_mutex_lock");
								   exit(1);
							   }

							   globalMem[ibyte3] = cur[worker]->reg[ibyte2];
							   /*if (ibyte2 == 7){printf("%d\n", (int)globalMem[ibyte3]&0xff);}*/
							   cur[worker]->pc += 3;

							   if(pthread_mutex_unlock(&cs_mtx)) {
								   perror("pthread_mutex_unlock");
								   exit(1);
							   }

							   break;
						   }
				case 0x08: {   // GSTOREi
							   if(pthread_mutex_lock(&cs_mtx)) {
								   perror("pthread_mutex_lock");
								   exit(1);
							   }

							   globalMem[ibyte3 + cur[worker]->reg[0]] = cur[worker]->reg[ibyte2];
							   cur[worker]->pc += 3;

							   if(pthread_mutex_unlock(&cs_mtx)) {
								   perror("pthread_mutex_unlock");
								   exit(1);
							   }

							   break;
						   }
				case 0x09: {   // SET
							   cur[worker]->reg[ibyte2] = (signed char)ibyte3;
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x0a: {   // ADD
							   cur[worker]->reg[ibyte2] = cur[worker]->reg[ibyte2] + cur[worker]->reg[ibyte3];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x0b: {   // SUB
							   cur[worker]->reg[ibyte2] = cur[worker]->reg[ibyte2] - cur[worker]->reg[ibyte3];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x0c: {   // MUL
							   cur[worker]->reg[ibyte2] = cur[worker]->reg[ibyte2] * cur[worker]->reg[ibyte3];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x0d: {   // DIV
							   cur[worker]->reg[ibyte2] = cur[worker]->reg[ibyte2] / cur[worker]->reg[ibyte3];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x0e: {   // MOD
							   cur[worker]->reg[ibyte2] = cur[worker]->reg[ibyte2] % cur[worker]->reg[ibyte3];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x0f: {   // BRGZ
							   if (cur[worker]->reg[ibyte2] > 0) { cur[worker]->pc+=3*(signed char)ibyte3; }
							   else { cur[worker]->pc+=3; }
							   break;
						   }
				case 0x10: {   // BRGEZ
							   if (cur[worker]->reg[ibyte2] >= 0) { cur[worker]->pc+=3*(signed char)ibyte3; }
							   else { cur[worker]->pc+=3; }
							   break;
						   }
				case 0x11: {   // BRLZ
							   if (cur[worker]->reg[ibyte2] < 0) { cur[worker]->pc+=3*(signed char)ibyte3; }
							   else { cur[worker]->pc+=3; }
							   break;
						   }
				case 0x12: {   // BRLEZ
							   if (cur[worker]->reg[ibyte2] <= 0) { cur[worker]->pc+=3*(signed char)ibyte3; }
							   else { cur[worker]->pc+=3; }
							   break;
						   }
				case 0x13: {   // BREZ
							   if (cur[worker]->reg[ibyte2] == 0) { cur[worker]->pc+=3*(signed char)ibyte3; }
							   else { cur[worker]->pc+=3; }
							   break;
						   }
				case 0x14: {   // BRA
							   cur[worker]->pc+=3*(signed char)ibyte3;
							   break;
						   }
				case 0x15: {   // DOWN
							   if(pthread_mutex_lock(&cs_mtx)) {
								   perror("pthread_mutex_lock");
								   exit(1);
							   }

							   globalMem[ibyte3]--;
							   if (globalMem[ibyte3] < 0) {
								   num_of_blocked++;
								   cur[worker]->state = BLOCKED;
								   cur[worker]->sem = ibyte3;
								   cur_copy = cur[worker];
							   }
							   cur[worker]->pc += 3;

							   if(pthread_mutex_unlock(&cs_mtx)) {
								   perror("pthread_mutex_unlock");
								   exit(1);
							   }

							   break;
						   }
				case 0x16: {   // UP
							   if(pthread_mutex_lock(&cs_mtx)) {
								   perror("pthread_mutex_lock");
								   exit(1);
							   }


							   globalMem[ibyte3]++;
							   if (globalMem[ibyte3] <= 0) {
								   num_of_blocked--;
								   // locate blocked tasks from a worker and move it to ready list
								   list_locate_move_blocked(ibyte3);
							   }
							   cur[worker]->pc += 3;

							   if(pthread_mutex_unlock(&cs_mtx)) {
								   perror("pthread_mutex_unlock");
								   exit(1);
							   }
							   break;
						   }
				case 0x17: {   // YIELD
							   cur[worker]->pc +=3;
							   break;
						   }
				case 0x18: {   // SLEEP
							   cur[worker]->state = SLEEPING;
							   cur[worker]->waket = time(NULL) + cur[worker]->reg[ibyte2];
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x19: {   // PRINT
							   printf("%d: %s\n", cur[worker]->id, &globalMem[ibyte3]);
							   cur[worker]->pc += 3;
							   break;
						   }
				case 0x1A: {   // EXIT
							   num_of_stopped++;
							   cur[worker]->state = STOPPED;
							   cur_copy = cur[worker];
							   tasks_stopped++;
							   break;
						   }
				default: {
							 printf("Error: Unknown instruction id\n");
							 exit(1);
						 }
			}
			// we dont want to continue with an instruction if cur got either stopped, blocked, or yielded proccessor
			if ((ibyte1 == 0x17) || (cur[worker]->state == STOPPED) || (cur[worker]->state == BLOCKED)) {break;}
			i++;
		}
	}
}
