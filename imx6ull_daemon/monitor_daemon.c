#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <syslog.h>
#include <ctype.h>
#include <time.h>

#define PID_FILE "/tmp/monitor_daemon.pid"   // 示例 PID 文件，生产环境改为 /var/run/... 
#define CHECK_INTERVAL_SEC 10                // 检查间隔（秒），可按需改 
#define RESTART_MAX_TRIES 3                  // 单次故障最大重启尝试次数 
#define SERVICE_NAME_MAX 128
#define CMD_MAX 512

// 服务描述结构体 
typedef struct service {
    char name[SERVICE_NAME_MAX];    // 用于匹配 /proc/.../cmdline 的关键字或可执行名 
    char start_cmd[CMD_MAX];        // 启动服务的 shell 命令
    int restart_tries;             // 连续重启尝试计数 
} service_t;

// 全局服务列表
service_t services[] = {
    // name 用于在 /proc/<pid>/cmdline 中查找包含 name 的进程；start_cmd 为启动命令 
    { .name = "mqttClient", .start_cmd = "/root/mqttClient" , .restart_tries = 0 },
    { .name = "lvglTest",    .start_cmd = "/root/lvglTest" , .restart_tries = 0 },
};
const int service_count = sizeof(services) / sizeof(services[0]);

// 程序运行控制变量 
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload = 0;

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsyslog(LOG_INFO, fmt, ap);
    va_end(ap);
}
void log_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsyslog(LOG_ERR, fmt, ap);
    va_end(ap);
}

// 写 PID 文件（覆盖） 
int write_pidfile(const char *pidfile) {
    FILE *f = fopen(pidfile, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

// 删除 PID 文件 
void remove_pidfile(const char *pidfile) {
    unlink(pidfile);
}

// 信号处理函数 
void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        g_running = 0;  // 优雅退出 
    } else if (sig == SIGHUP) {
        g_reload = 1;   // 触发重新加载
    } else if (sig == SIGCHLD) {
        // 子进程终止：回收，避免僵尸进程 
        while (waitpid(-1, NULL, WNOHANG) > 0) {
        }
    }
}

/*
 * daemonize - 标准守护化流程
 */
int daemonize(void) {
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        //父进程退出
        exit(0);
    }

    if (setsid() < 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        //第一次子进程退出，让第二次子进程成为最终守护进程
        exit(0);
    }

    //设置文件权限掩码、工作目录、关闭文件描述符
    umask(0);
    chdir("/");

    // 关闭传入的文件描述符（保留 stdin/stdout/stderr 也可重定向到 /dev/null） 
    for (int fd = sysconf(_SC_OPEN_MAX); fd >= 0; fd--) {
        close(fd);
    }

    // 重新打开标准描述符指向 /dev/null
    // int fd = open("/dev/null", O_RDWR); // 0 - stdin 
    // dup2(fd, 0);
    // dup2(fd, 1);
    // dup2(fd, 2);

    int fd = open("/tmp/log.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    dup2(fd, 1); // stdout
    dup2(fd, 2); // stderr

    int fdnull = open("/dev/null", O_RDONLY);
    dup2(fdnull, 0);  // stdin
    //setenv("PATH", "/usr/bin:/bin:/usr/local/bin", 1);
    //setenv("LD_LIBRARY_PATH", "/usr/lib:/usr/local/lib", 1);

    return 0;
}

// 检查路径是否为数字（/proc 中 pid 目录） 
static int is_numeric_dir(const char *name) {
    for (; *name; ++name) {
        if (!isdigit((unsigned char)*name)) return 0;
    }
    return 1;
}

/*
 * find_pid_by_name
 *  遍历 /proc，读取每个进程的 /proc/<pid>/cmdline 文件，
 *  若 cmdline 中包含关键词 name_substr，则返回该 pid（第一个匹配）。
 *  返回 >0 的 pid 表示找到，返回 0 表示未找到，返回 -1 表示错误。
 *
 */
pid_t find_pid_by_name(const char *name_substr) {
    DIR *proc = opendir("/proc");
    if (!proc) return -1;

    struct dirent *entry;
    char path[256];
    char buf[4096];

    while ((entry = readdir(proc)) != NULL) {
        if (!is_numeric_dir(entry->d_name)) continue;

        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (n == 0) continue;
        buf[n] = '\0';

        // /proc/<pid>/cmdline 中参数间用 '\0' 分隔，方便用 strstr 
        if (strstr(buf, name_substr) != NULL) {
            pid_t pid = (pid_t)atoi(entry->d_name);
            closedir(proc);
            return pid;
        }
    }

    closedir(proc);
    return 0;
}

// 启动服务（通过 shell command），返回子进程 pid 或 -1 出错 
//pid_t start_service_cmd(char *cmd){//(const service_t *s) {
pid_t start_service_cmd(const service_t *s) {
    pid_t pid = fork();
    if (pid < 0) {
        log_err("fork failed when starting service: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        // 子进程：执行 shell 启动命令 
        // 注意：这里不做守护化，假设被启动的程序自行处理或由此进程监控 
        //execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        execl(s->start_cmd, s->name, NULL);
        // 若 execl 返回，发生错误 
        _exit(127);
    }
    // 父进程返回子 pid 
    return pid;
}

// 服务检查与重启逻辑 
void check_and_recover_services(void) {
    for (int i = 0; i < service_count; ++i) {
        service_t *s = &services[i];
        pid_t pid = find_pid_by_name(s->name);
        if (pid > 0) {
            // 进程存在，重置重试计数（如果之前尝试过） 
            if (s->restart_tries != 0) {
                log_info("Service '%s' is back (pid=%d). Reset restart_tries.", s->name, pid);
                s->restart_tries = 0;
            }
            // 可在此添加更复杂的健康检查（端口、MQTT 心跳等） 
        } else if (pid == 0) {
            // 未找到进程，尝试重启 
            s->restart_tries++;
            log_info("Service '%s' not running. Attempting restart (%d/%d). Command: %s",
                     s->name, s->restart_tries, RESTART_MAX_TRIES, s->start_cmd);

            //pid_t child = start_service_cmd(s->start_cmd);
            pid_t child = start_service_cmd(s);
            if (child > 0) {
                log_info("Started service '%s' as pid %d", s->name, child);
            } else {
                log_err("Failed to start service '%s' (fork/execl failed).", s->name);
            }

            if (s->restart_tries >= RESTART_MAX_TRIES) {
                log_err("Service '%s' failed to start after %d attempts — will wait longer before next try.",
                        s->name, s->restart_tries);
            }
        } else {
            log_err("Error checking service '%s': find_pid_by_name returned -1", s->name);
        }
    }
}

// 在 SIGHUP 时重新加载配置文件
void reload_config(void) {
    log_info("SIGHUP received: reload configuration.");
    // 在这里读取 /etc/monitor_daemon.conf 之类的文件，更新 services[] 配置 
}

// 主循环 
void monitor_loop(void) {
    while (g_running) {
        if (g_reload) {
            reload_config();
            g_reload = 0;
        }
        check_and_recover_services();

        // 休眠 CHECK_INTERVAL_SEC 秒
        for (int i = 0; i < CHECK_INTERVAL_SEC && g_running; ++i) {
            sleep(1);
        }
    }
}

int main(int argc, char *argv[]) {
    // 打开 syslog
    openlog("monitor_daemon", LOG_PID | LOG_CONS, LOG_DAEMON);

    // 简单的互斥：如果已有 PID 文件，提示并退出
    FILE *pf = fopen(PID_FILE, "r");
    if (pf) {
        int existing_pid = 0;
        if (fscanf(pf, "%d", &existing_pid) == 1) {
            if (existing_pid > 0) {
                log_err("Pidfile %s exists with pid %d. Exiting to avoid duplicate daemon.", PID_FILE, existing_pid);
                fclose(pf);
                closelog();
                fprintf(stderr, "Pidfile exists with pid %d. Exiting.\n", existing_pid);
                return 1;
            }
        }
        fclose(pf);
    }

    
    if (daemonize() != 0) {
        // 无法守护化，仍可继续在前台运行，视情况而定 
        log_err("daemonize failed: %s", strerror(errno));
    }

    // 写 PID 文件 
    if (write_pidfile(PID_FILE) != 0) {
        log_err("Failed to write pidfile %s: %s", PID_FILE, strerror(errno));
        // 继续运行也可，但建议退出或警告 
    } else {
        log_info("Wrote pidfile %s", PID_FILE);
    }

    // 安装信号处理 
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; // SA_NOCLDSTOP 防止子进程 stop 时触发 SIGCHLD 
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    // 主监控循环 
    log_info("Monitor daemon started. Checking interval %d seconds.", CHECK_INTERVAL_SEC);
    monitor_loop();

    // 退出清理 
    log_info("Monitor daemon shutting down.");
    remove_pidfile(PID_FILE);
    closelog();

    return 0;
}
