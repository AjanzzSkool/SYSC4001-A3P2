#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/sem.h>

using namespace std;

#define RUBRIC_LINES 5


#define RUBRIC_LINE_LEN 100

//semaphores
enum {
    SEM_RUBRIC = 0,
    SEM_QUESTIONS = 1,
    SEM_EXAMS = 2
};

int semid;

// A union thats needed for semctl
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

//wait if another TA has access
void sem_wait_idx(int idx) {
    struct sembuf op;
    op.sem_num = idx;
    op.sem_op  = -1;
    op.sem_flg = 0;
    if (semop(semid, &op, 1) == -1) {
        perror("semop wait");
        exit(1);
    }
}

//signal that TA is done and allow in any waiting TA
void sem_signal_idx(int idx) {
    struct sembuf op;
    op.sem_num = idx;
    op.sem_op  = 1;
    op.sem_flg = 0;
    if (semop(semid, &op, 1) == -1) {
        perror("signal semop");
        exit(1);
    }
}

//The shared memory that all of the TA has access to
struct SharedData {
    char rubric[RUBRIC_LINES][RUBRIC_LINE_LEN];
    int studentNumber;
    int questionStatus[RUBRIC_LINES];
    int currentExam;
    int totalExams;
};

//reading the rubric and storing it into the shared memory
void load_rubric(SharedData *shared, const string &rubricFile) {
    ifstream fin(rubricFile);
    string line;
    for (int i = 0; i < RUBRIC_LINES; i++) {
        getline(fin, line);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        strncpy(shared->rubric[i], line.c_str(), RUBRIC_LINE_LEN - 1);
        shared->rubric[i][RUBRIC_LINE_LEN - 1] = '\0';
    }
}

//save changes that TA made  to rubric
void save_rubric(SharedData *shared, const string &rubricFile) {
    ofstream fout(rubricFile);
    for (int i = 0; i < RUBRIC_LINES; i++)
        fout << shared->rubric[i] << '\n';
}

//loads up a new exam and unmarks all the questions in it
void load_exam(SharedData *shared, const string &examFile) {
    ifstream fin(examFile);
    string line;
    getline(fin, line);
    shared->studentNumber = atoi(line.c_str());
    for (int i = 0; i < RUBRIC_LINES; i++)
        shared->questionStatus[i] = 0;
    cout << " Loaded exam " << shared->currentExam
         << " (student " << shared->studentNumber << ") fom "
         << examFile << "\n";
}

//the random delay
void random_sleep_ms(int minMs, int maxMs) {
    int delay = minMs + (rand() % (maxMs - minMs + 1));
    usleep(delay * 1000);
}

//ta goes through rubric and has a 1/2 chance of changing the rubric line
void ta_review_rubric(int taId, SharedData *shared, const string &rubricFile) {
    //Only one ta can change rubric at one time
    sem_wait_idx(SEM_RUBRIC);

    for (int i = 0; i < RUBRIC_LINES; i++) {
        cout << "TA " << taId << ": Checking rubric line " << (i + 1)
             << ": " << shared->rubric[i] << "\n";
        random_sleep_ms(500, 1000);
        bool change = (rand() % 2 == 0);
        if (change) {
            char *comma = strchr(shared->rubric[i], ',');
            if (comma && comma[1] == ' ' && comma[2] != '\0') {
                comma[2] = comma[2] + 1;
                cout << "[TA " << taId << "] Corrected rubric line "
                     << (i + 1) << " to: " << shared->rubric[i] << "\n";
                save_rubric(shared, rubricFile);
            }
        }
    }

    sem_signal_idx(SEM_RUBRIC);
}

void ta_mark_exam(int taId, SharedData *shared) {
    int student = shared->studentNumber;
    while (true) {
        int qIndex = -1;

        // choose a question that is not started yet
        sem_wait_idx(SEM_QUESTIONS);
        for (int i = 0; i < RUBRIC_LINES; i++) {
            if (shared->questionStatus[i] == 0) {
                qIndex = i;
                shared->questionStatus[i] = 1; // in progress
                break;
            }
        }
        sem_signal_idx(SEM_QUESTIONS);

        if (qIndex == -1)
            break;

        random_sleep_ms(1000, 2000);

        // mark it as done
        sem_wait_idx(SEM_QUESTIONS);
        shared->questionStatus[qIndex] = 2;
        sem_signal_idx(SEM_QUESTIONS);

        cout << "TA " << taId << " Marked qustion " << (qIndex + 1) << " for student " << student << "\n";
    }
}

//the main loop for ta processes
void ta_process(int taId, SharedData *shared,const vector<string> &examFiles,const string &rubricFile) {

//makes sure that each process gets its own random number when the random delay is called
    srand(time(nullptr) ^ getpid());
    while (true) {
        if (shared->studentNumber == 9999) {
            cout << "TA " << taId << ": student 9999 reached so stopping.\n";
            break;
        }

        int student = shared->studentNumber;
        cout << "TA " << taId << ": Working on exam # "
             << shared->currentExam << " (student " << student << ")\n";

        ta_review_rubric(taId, shared, rubricFile);
        ta_mark_exam(taId, shared);

        bool allDone = true;

        // exam completion and moving to next exam has to be synced
        sem_wait_idx(SEM_EXAMS);

        for (int i = 0; i < RUBRIC_LINES; i++) {
            if (shared->questionStatus[i] != 2) {
                allDone = false;
                break;
            }
        }

        if (allDone) {
            if (shared->currentExam + 1 < shared->totalExams) {
                shared->currentExam++;
                load_exam(shared, examFiles[shared->currentExam]);
            } else {
                shared->studentNumber = 9999;
            }
        }

        sem_signal_idx(SEM_EXAMS);

        if (!allDone) {
            random_sleep_ms(300, 600);
        }
    }
}

int main(int argc, char *argv[]) {
    int numTAs = atoi(argv[1]);
    string examListFile = argv[2], rubricFile = argv[3];

    vector<string> examFiles;
    ifstream elist(examListFile);
    string line;
// get all the exam paths
    while (getline(elist, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty())
            examFiles.push_back(line);
    }

//make the shared memory
    int shmid = shmget(IPC_PRIVATE, sizeof(SharedData), IPC_CREAT | 0666);
    SharedData *shared = (SharedData *)shmat(shmid, nullptr, 0);

    memset(shared, 0, sizeof(SharedData));
    shared->currentExam = 0;
    shared->totalExams = examFiles.size();

    load_rubric(shared, rubricFile);
    load_exam(shared, examFiles[0]);


    semid = semget(IPC_PRIVATE, 3, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("semget");
        exit(1);
    }
    union semun arg;
    unsigned short values[3] = {1, 1, 1};
// Start everything as unlocked
    arg.array = values;
    if (semctl(semid, 0, SETALL, arg) == -1) {
        perror("semctl setall faled");
        exit(1);
    }
//make a process for each Ta
    for (int i = 0; i < numTAs; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            SharedData *childShared = (SharedData *)shmat(shmid, nullptr, 0);
            ta_process(i + 1, childShared, examFiles, rubricFile);
            shmdt(childShared);
            exit(0);
        }
    }
//wait until all the child TA pocesses end then do cleanup
    for (int i = 0; i < numTAs; i++)
        wait(nullptr);

    shmdt(shared);
    shmctl(shmid, IPC_RMID, nullptr);
    semctl(semid, 0, IPC_RMID);

    cout << "All tas finished.\n";
    return 0;
}
