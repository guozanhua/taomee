/* vim: set expandtab tabstop=4 softtabstop=4 shiftwidth=4: */
/**
 * @file  newbench.cpp
 * @author tonyliu <tonyliu@taomee.com>
 */

#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "newbench.h"
#include "newbench_util.h"
#include "i_ring_queue.h"
#include "i_barrier_ring_queue.h"
#include "afutex.h"
#include "net_io_notifier.h"

#include "dll.h"
#include "daemon.h"
#include "util.h"
#include "net_proc.h"
#include "work_proc.h"

using namespace std;

#define NEWBENCH_VERSION "2.0.4"

i_barrier_ring_queue *g_prcv_brq = NULL;
i_ring_queue *g_prcv_rq = NULL;
i_ring_queue *g_psnd_rq = NULL;

bench_config_t g_bench_conf;

/// 对环形队列互斥的锁
static char* g_rq_lock_shm = NULL;
static int g_rq_lock_shm_len = 0;
int* g_snd_rq_lock = NULL;
int* g_rcv_rq_lock = NULL;
afutex_opbuf_t* g_rcv_wait_op = NULL;

i_ini_file *g_p_ini = NULL;
i_net_io_notifier *g_p_net_io_notifier = NULL;

extern "C" int log_init(const char* dir, log_lvl_t lvl, uint32_t size, int maxfiles, const char* pre_name);
extern void setproctitle_init(char** main_argv);

/**
 * @brief  参数解析
 */
int parse_args(int argc, const char* const argv[])
{
    if (argc != 2) {
        print_prompt(false, "Usage: %s conf_file", argv[0]);
        return -1;
    }
    g_bench_conf.saved_argv = (char**)malloc(sizeof(char *) * (argc + 1));
    if (g_bench_conf.saved_argv == NULL) {
        print_prompt(false, "Failed to malloc space to save argv.");
        return -1;
    }
    dup_argv(argc, argv, g_bench_conf.saved_argv);
    
    return 0;
}

int config_init()
{
    int rv = 0;
    char buff[4096] = {0};
    
    rv = g_p_ini->read_string("RunInfo", "prog_name", buff, sizeof(buff), g_bench_conf.saved_argv[0]);
    if (rv != 0) {
        print_prompt(false, "Get RunInfo prog_name failed(%d).", rv);
    }
    g_bench_conf.prog_name = strdup(buff);
    
    rv = g_p_ini->read_string("RunInfo", "working_dir", buff, sizeof(buff), get_current_dir_name());
    if (rv != 0) {
        print_prompt(false, "Get RunInfo working_dir failed(%d).", rv);
    }
    g_bench_conf.working_dir = strdup(buff);
    
    if (chdir(g_bench_conf.working_dir) != 0) {
        print_prompt(false, "ERROR: chdir(%s): %s", g_bench_conf.working_dir, strerror(errno));
        return -1;
    }

    // 初始化log日志
    rv = g_p_ini->read_string("LogInfo", "log_dir", g_bench_conf.log_dir, MAX_CONF_STR_LEN, NULL);
    if (rv != 0) {
        print_prompt(false, "Get log_dir config failed(%d).", rv);
        return -1;
    }
    g_bench_conf.log_level = g_p_ini->read_int("LogInfo", "log_level", log_lvl_trace);
    g_bench_conf.log_size = g_p_ini->read_int("LogInfo", "log_size", 1 << 30);
    g_bench_conf.log_maxfiles = g_p_ini->read_int("LogInfo", "log_maxfiles", 0);
    rv = g_p_ini->read_string("LogInfo", "log_prefix", g_bench_conf.log_prefix, MAX_CONF_STR_LEN, "");
    if (rv != 0) {
        print_prompt(false, "Get log_prefix config failed(%d).", rv);
        return -1;
    }

    // 初始化ConnInfo
    rv = g_p_ini->read_string("ConnInfo", "bind_file", g_bench_conf.bind_file, MAX_CONF_STR_LEN, NULL);
    if (rv != 0) {
        print_prompt(false, "Get bind_file config failed(%d)", rv);
        return -1;
    }
    rv = load_bind_file(g_bench_conf.bind_file, &g_bench_conf.bind_list);
    if (rv != 0) {
        print_prompt(false, "Load bind file: %s", g_bench_conf.bind_file);
        return -1;
    }
    print_prompt(true, "Load bind file: %s", g_bench_conf.bind_file);
    g_bench_conf.use_barrier = g_p_ini->read_int("RunInfo", "use_barrier", 0) == 0 ? false : true;

    // 初始化WorkInfo
    rv = g_p_ini->read_string("WorkInfo", "dll", g_bench_conf.so_file, MAX_CONF_STR_LEN, NULL);
    if (rv != 0) {
        print_prompt(false, "Get dll config failed(%d)!", rv);
        return -1;
    }
    g_bench_conf.shmq_length = g_p_ini->read_int("RunInfo", "shmq_length", 1 << 26);
    NB_CHECK_INT("shmq_length", g_bench_conf.shmq_length);

    g_bench_conf.pkg_timeout = g_p_ini->read_int("RunInfo", "pkg_timeout", 10);
    NB_CHECK_INT("pkg_timeout", g_bench_conf.pkg_timeout);//待定

    g_bench_conf.worker_num = g_p_ini->read_int("WorkInfo", "worker_num", 1);
    NB_CHECK_INT("worker_num", g_bench_conf.worker_num);

    g_bench_conf.max_pkg_len = g_p_ini->read_int("RunInfo", "max_pkg_len", MAX_PACKAGE_LEN);
    NB_CHECK_INT("max_pkg_len", g_bench_conf.max_pkg_len);

    g_bench_conf.avg_pkg_len = g_p_ini->read_int("RunInfo", "avg_pkg_len", 32);
    NB_CHECK_INT("avg_pkg_len", g_bench_conf.avg_pkg_len);

    print_prompt(true, "Max package length: %u B", g_bench_conf.max_pkg_len);

    return 0;
}

void config_dump()
{
    char buf[MAX_CONF_STR_LEN * 2 * 20] = {0};
    sprintf(buf, "Dump config info: \n");
    sprintf(buf + strlen(buf), "  [RunInfo]\n");
    sprintf(buf + strlen(buf), "    prog_name = %s\n", g_bench_conf.prog_name);
    sprintf(buf + strlen(buf), "    working_dir = %s\n", g_bench_conf.working_dir);
    sprintf(buf + strlen(buf), "    use_barrier = %u\n", g_bench_conf.use_barrier);
    sprintf(buf + strlen(buf), "    max_pkg_len = %u\n", g_bench_conf.max_pkg_len);
    sprintf(buf + strlen(buf), "    avg_pkg_len = %u\n", g_bench_conf.avg_pkg_len);
    sprintf(buf + strlen(buf), "    shmq_length = %u\n", g_bench_conf.shmq_length);
    sprintf(buf + strlen(buf), "    pkg_timeout = %u\n", g_bench_conf.pkg_timeout);

    sprintf(buf + strlen(buf), "  [LogInfo]\n");
    sprintf(buf + strlen(buf), "    log_dir = %s\n", g_bench_conf.log_dir);
    sprintf(buf + strlen(buf), "    log_level = %u\n", g_bench_conf.log_level);
    sprintf(buf + strlen(buf), "    log_size = %u\n", g_bench_conf.log_size);
    sprintf(buf + strlen(buf), "    log_maxfiles = %u\n", g_bench_conf.log_maxfiles);
    sprintf(buf + strlen(buf), "    log_prefix = %s\n", g_bench_conf.log_prefix);

    sprintf(buf + strlen(buf), "  [ConnInfo]\n");
    sprintf(buf + strlen(buf), "    bind_file = %s\n", g_bench_conf.bind_file);

    sprintf(buf + strlen(buf), "  [WorkInfo]\n");
    sprintf(buf + strlen(buf), "    dll = %s\n", g_bench_conf.so_file);
    sprintf(buf + strlen(buf), "    worker_num = %u", g_bench_conf.worker_num);

    TRACE_LOG("%s", buf);

    TRACE_LOG("[Bind file information]");
    bind_dump(g_bench_conf.bind_list);
}

int nb_init()
{
    if (create_ini_file_instance(&g_p_ini) != 0) {
        print_prompt(false, "Create i_ini_file failed.");
        return -1;
    }

    if (g_p_ini->init(g_bench_conf.saved_argv[1]) != 0) {
        print_prompt(false, "Init ini file[%s] failed!", g_bench_conf.saved_argv[1]);
        return -1;
    }

    print_prompt(true, "Load config file: %s", g_bench_conf.saved_argv[1]);

    if (config_init() != 0) {
        return -1;
    }

    log_init(g_bench_conf.log_dir, (log_lvl_t)g_bench_conf.log_level, g_bench_conf.log_size, 0,
             g_bench_conf.log_prefix);

    // 加载SO文件
    if (register_plugin(g_bench_conf.so_file) != 0) {
        print_prompt(false, "Load dll file: %s ", g_bench_conf.so_file);
        return -1;
    }
    print_prompt(true, "Load dll file: %s", g_bench_conf.so_file);
    if (g_bench_conf.use_barrier && dll.handle_dispatch == NULL) {
        print_prompt(true, "Warnning: handle_dispatch not found, pkg filter disabled.");
    }

    // 检查SO中必须实现的接口是否已经实现
    if (dll.handle_input == NULL) {
        print_prompt(false, "dll handle_input is null!");
        return -1;
    }

    if (dll.handle_process == NULL) {
        print_prompt(false, "dll handle_process is null!");
        return -1;
    }

    // 初始化数据包接受环形队列
    if (g_bench_conf.use_barrier) {///barrier ring queue
        if (create_barrier_ring_queue_instance(&g_prcv_brq) != 0) {
            print_prompt(false, "Create i_barrier_ring_queue failed.");
            return -1;
        }

        // 注：平均包长中要包含shm_block_t的长度，因为每个数据包都会加上这个头以后
        //     再压入过滤器！
        if (g_prcv_brq->init(g_bench_conf.shmq_length, g_bench_conf.avg_pkg_len + sizeof(shm_block_t)) != 0) {
            print_prompt(false, "Init barrier failed: %s", g_prcv_brq->get_last_errstr());
            return -1;
        }
        print_prompt(true, "Init barrier ring queue, size %u KB, avg_pkg_len %u B + %zd B",
            g_bench_conf.shmq_length/1024, g_bench_conf.avg_pkg_len, sizeof(shm_block_t));
    } else { // common ring queue
        if (create_variable_queue_instance(&g_prcv_rq, sizeof(int32_t)) != 0) {
            print_prompt(false, "Create receive i_ring_queue failed.");
            return -1;
        }

        if (g_prcv_rq->init(g_bench_conf.shmq_length) != 0) {
            print_prompt(false, "Init receive i_ring_queue failed: %d", g_prcv_rq->get_last_errno());
            return -1;
        }
        print_prompt(true, "Init receive ring queue, size %u KB", g_bench_conf.shmq_length/1024);
    }

    // 初始化数据包发送环形队列
    if (create_variable_queue_instance(&g_psnd_rq, sizeof(int32_t)) != 0) {
        print_prompt(false, "Create send i_ring_queue failed.");
        return -1;
    }
    if (g_psnd_rq->init(g_bench_conf.shmq_length) != 0) {
        print_prompt(false, "Init send i_ring_queue failed: %d", g_psnd_rq->get_last_errno());
        return -1;
    }
    print_prompt(true, "Init send ring queue: %d KB", g_bench_conf.shmq_length/1024);

    // 初始化环形队列互斥的锁
    g_rq_lock_shm_len = getpagesize();
    if ((u_int)g_rq_lock_shm_len < sizeof(int) * 2 + sizeof(afutex_opbuf_t)) {
        g_rq_lock_shm_len = sizeof(int) * 2 + sizeof(afutex_opbuf_t);
    }
    g_rq_lock_shm = (char *)mmap(NULL, g_rq_lock_shm_len, PROT_WRITE | PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_rq_lock_shm == MAP_FAILED) {
        print_prompt(false, "Mmap ring queue lock shm fail.");
        return -1;
    }
    g_rcv_rq_lock = (int*)g_rq_lock_shm;
    g_snd_rq_lock = (int*)(g_rq_lock_shm + sizeof(int));
    g_rcv_wait_op = (afutex_opbuf_t*)(g_rq_lock_shm + sizeof(int) * 2);

    afutex_mutex_init(g_rcv_rq_lock);
    afutex_mutex_init(g_snd_rq_lock);
    afutex_opbuf_init(g_rcv_wait_op);

    if (net_io_notifier_create(&g_p_net_io_notifier)) {
        print_prompt(false, "fail to create net_io_notifier!");
        return -1;
    }

    if (g_p_net_io_notifier->init()) {
        print_prompt(false, "fail to init net_io_notifier!");
        return -1;
    }

    config_dump();
    
    return 0;
}

int nb_uninit()
{
    // 收回共享内存
    if (g_rq_lock_shm != NULL && g_rq_lock_shm_len > 0) {
        munmap(g_rq_lock_shm, g_rq_lock_shm_len);
    }

    NB_CLASS_FREE(g_prcv_brq);
    NB_CLASS_FREE(g_prcv_rq);
    NB_CLASS_FREE(g_psnd_rq);
    free_bind_file(g_bench_conf.bind_list);
    NB_CLASS_FREE(g_p_ini);
    NB_COMMON_FREE(g_bench_conf.prog_name);
    NB_COMMON_FREE(g_bench_conf.working_dir);
    free_argv(g_bench_conf.saved_argv);

    return 0;
}

int main(int argc, char* argv[])
{
    print_prompt(true, "newbench: version: "NEWBENCH_VERSION" build time: " __DATE__" "__TIME__);

    if (daemon_start() != 0) {
        print_prompt(false, "daemon_start");
        return -1;
    }
    
    memset(&g_bench_conf, 0, sizeof(bench_config_t));
    if (parse_args(argc, argv) != 0) {
        print_prompt(false, "parse_args");
        return -1;
    }

    if (nb_init() != 0) {
        print_prompt(false, "nb_init");
        return -1;
    }
    
    // 初始化设置proc标题
    setproctitle_init(argv);

    // 设置proc的标题，方便ps时查看进程
    setproctitle("-%s:[MAIN]", g_bench_conf.prog_name);

    if (dll.handle_init) {
        dll.handle_init(argc, argv, PROC_MAIN);
    }

    if (g_bench_conf.use_barrier && dll.handle_dispatch == NULL) {
        TRACE_LOG("ERROR: Warning: handle_dispatch not found, pkg filter disabled.");
        return -1;
    }

    if (tell_wait() != 0) {
        print_prompt(false, "ERROR: tell_wait");
        return -1;
    }

    pid_t net_proc_pid = spawn_net_proc(argc, g_bench_conf.saved_argv);
    if (net_proc_pid == -1) {
        print_prompt(false, "ERROR: spawn_net_proc");
        return -1;
    }
    print_prompt(true, "spawned net proc");

    for (u_int i = 0; i < g_bench_conf.worker_num; ++i) {
        if (spawn_work_proc(argc, g_bench_conf.saved_argv) == -1) {
            print_prompt(false, "ERROR: spawn_work_proc");
            return -1;
        }
    }
    print_prompt(true, "spawned %u work proc", g_bench_conf.worker_num);

    int sec = 10;
    time_t last_check_time = time(NULL);
    bool stop = false;
    while (!stop) {
        sleep(1);
        
        time_t current_time = time(NULL);
        if (current_time - last_check_time >= sec) {
            last_check_time = current_time;
            if (dll.handle_timer) {
                dll.handle_timer(&sec);
            }
        }

        int status = 0;
        int exit_status = 0;
        int term_sig = 0;

        for (;;) {
            pid_t pid = waitpid(0, &status, WNOHANG);
            if (pid == 0) {
                break;
            } else if (pid == -1 && errno == ECHILD) {
                TRACE_LOG("has no more child, so exit");
                stop = true;
                break;
            } else if (pid == -1 && errno == EINTR) {
                continue;
            }

            TRACE_LOG("child process %d exit", (int)pid);

            if (WIFEXITED(status)) {
                exit_status = WEXITSTATUS(status);
                TRACE_LOG("child process %d exited with status %d.", (int)pid, exit_status);
                if (exit_status == 0) {
                    // normal exit
                    continue;
                }   
            } else if (WIFSIGNALED(status)) {
                term_sig = WTERMSIG(status);
                TRACE_LOG("ERROR: child process %d killed by signal %d.", (int)pid, term_sig);
            } else {
                TRACE_LOG("ERROR: child process %d stopped!?", (int)pid);
            } 

            // reboot child proc
            TRACE_LOG("ERROR: child process %d abnormal exit", (int)pid);
            if (pid == net_proc_pid) {
                // reboot net proc
                TRACE_LOG("reboot net proc");

                net_proc_pid = spawn_net_proc(argc, g_bench_conf.saved_argv);
                if (net_proc_pid == -1) {
                    print_prompt(false, "ERROR: spawn_net_proc");
                    return -1;
                }
            } else {
                // reboot work proc
                TRACE_LOG("reboot work proc");

                // work proc异常退出时，需要清理过滤器中这个进程的资源
                if (g_bench_conf.use_barrier) {
                    AFUTEX_LOCK_ERR_CHK(g_rcv_rq_lock);
                    g_prcv_brq->clean_4pid(pid);
                    AFUTEX_UNLOCK_ERR_CHK(g_rcv_rq_lock);
                }

                if (spawn_work_proc(argc, g_bench_conf.saved_argv) == -1) {
                    print_prompt(false, "ERROR: spawn_work_proc");
                    return -1;
                }
            }
        }
    }
    
    if (dll.handle_fini) {
        dll.handle_fini(PROC_MAIN);
    }

    nb_uninit();
    // 只能增加资源释放相关代码
    return 0;
}
