#include "Session.h"

#define PATHNAME "."
#define PATHNAME1 "../"
#define PROJ_ID 0x6666
#define PROJ_ID1 0x6667
static void DisplayOpensslVersion();
static key_t key = 0;
static key_t key1 = 0;
static const int sharesize = 4096*2; 
struct Session_Version Version={1,0,0};
static struct Session_Management *sessionmanagement;
static shmid = 0;
static bshmid = 0 ;
///////////////////////////////////////////////////////////////////////////////////////////
static int CommonShareMemory(int size,int flags);
static int semaphore_allocation(key_t key,int sem_flags)
{
    return semget(key,1,sem_flags);
}
static int semaphore_deallocate()
{
    union semun ignored_argument;
    return semctl(bshmid,1,IPC_RMID,ignored_argument);
}
static int semaphore_initialize()
{
    union semun argument;
    unsigned short values[1];
    values[0]=1;
    argument.array=values;
    return semctl(bshmid,0,SETALL,argument);
}
static int semaphore_wait()
{
    struct sembuf operations[1];
    operations[0].sem_num=0;
    operations[0].sem_op=-1;
    operations[0].sem_flg=SEM_UNDO;
    printf("waitting is %d\n",(int)getpid());
    return semop(bshmid,operations,1);
}
static int semaphore_post()
{
    struct sembuf operations[1];
    operations[0].sem_num=0;
    operations[0].sem_op=1;
    operations[0].sem_flg=SEM_UNDO;
    printf("process is %d\n",(int)getpid());
    return semop(bshmid,operations,1);
}
void DisplayKeyInfo(){
    printf("the key is %u\r\n",key);
    printf("the key1 is %u\r\n",key1);
}
int ShareMemoryInit(){
    key = ftok(PATHNAME,PROJ_ID);
    key1 = ftok(PATHNAME1,PROJ_ID1);
    //semaphore_allocation(key,IPC_CREAT |0666);
    //bshmid = semaphore_initialize(key1);
    DisplayKeyInfo();
    return CreatShareMemory();
}
int CreatShareMemory(){
    int ret = 0;
    do{
        ret = CommonShareMemory(sharesize,IPC_CREAT|IPC_EXCL|0666);
        printf('the ret value is %d\r\n',ret);
        if(ret<0){
            printf('clean  ret value\r\n',ret);
            DestoryShm(shmid);
        }
    }while(ret<0);

}
int GetShareMemory(){
    sessionmanagement = shmat(shmid,NULL,0);
}
int SetShareMemory(){
    CommonShareMemory(sharesize,IPC_CREAT);
}
int CommonShareMemory(int size,int flags){
    if(key<=0)
    {
        perror("ftok error\n");
        return -1;
    }
    if((shmid=shmget(key,size,flags))<0)
    {
        perror("shmget error\n");
        return -2;
    }
    return shmid;
}
int DestoryShm(int shmid)
{
    if(shmctl(shmid,IPC_RMID,NULL)<0)
    {
        perror("shmctl error\n");
        return -1;
    }
    return 0;
}
void DisplayVersion(){
    printf("The Session Version is %d.%d.%d\r\n",Version.Major,Version.Modify,Version.Id);
    DisplayOpensslVersion();
}
//////////////////////////////////////////////////////////////////////////////////////////////
void DisplayOpensslVersion(){
    printf("the openssl version is %s\r\n",OpenSSL_version(OPENSSL_VERSION));
}
void Display()
{
    int i = 0 ;
    struct SessionInfo *con;
    for(i;i < SESSION_NUM;i++ ){
        con = &sessionmanagement->sesssion[i] ;
        if(con->stat){
            printf("%d is used and stat is %d\r\n",i,con->stat);
            printf("the id is %u the token is %u\r\n",con->connect.ipaddr,con->connect.token);
            if(con->stat>1){
                printf("the user name is %s\r\n",con->user.username);
                printf("the user power is %d\r\n",con->user.power);
            }
        }
    }
}
int Add(struct ConnectInfo conn){
    //semaphore_wait();
    int i = sessionmanagement->min ;
    struct SessionInfo *con;
    if(sessionmanagement->num>=SESSION_NUM){
        return 0;
    }
    for(i;i < SESSION_NUM;i++ ){
        con = &sessionmanagement->sesssion[i] ;
        if(!con->stat){
            sessionmanagement->num++;
            con->stat = 1;
            con->connect.ipaddr = conn.ipaddr;
            con->connect.token = conn.token;
            sessionmanagement->num++;
            sessionmanagement->sesssion->index = i;
            sessionmanagement->min = i;
            if(i>sessionmanagement->max){
                sessionmanagement->max = i;
            }
            break;
        }
    }
    //semaphore_post();
    Display();
    if(i<=SESSION_NUM){
        return i;
    }else{
        return 0;
    }
}
int SetLogStat(unsigned int index,char *str){
    //semaphore_wait();
    struct SessionInfo *con;
    if(!str){
        printf("SetLogStat error Info\r\n");
        return 0;
    }
    if(sessionmanagement->num<=0){
        return 0;
    }
    if(index<0 && index>sessionmanagement->max){
        return 0;
    }
    con = &sessionmanagement->sesssion[index] ;
    if(con->stat){
        con->stat = 2;
        strncpy(con->user.username,str,PASSWORDLEN-1);
        con->timer = NULL;
        if(index<sessionmanagement->min){
            sessionmanagement->min = index;
        }
        printf("SetLogStat connected success\r\n");
        
    }else{
        printf("SetLogStat connected failed\r\n");
    }
    //semaphore_post();
    Display();
   return index;
}
int Del(unsigned int index){
    //semaphore_wait();
    struct SessionInfo *con;
    if(sessionmanagement->num<=0){
        return 0;
    }
    if(index<0&&index>sessionmanagement->max){
        return 0;
    }
    con = &sessionmanagement->sesssion[index] ;
    if(con->stat){
        sessionmanagement->num--;
        con->stat = 0;
        con->user.username[0]=0;
        con->timer = NULL;
        if(index<sessionmanagement->min){
            sessionmanagement->min = index;
        }
        printf("Del connected success\r\n");
        
    }else{
        printf("Del connected failed\r\n");
    }
    //semaphore_post();
    Display();
    return index;
}