/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2014, Joyent, Inc.
 */

#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <libcontract.h>
#include <libzonecfg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/contract/process.h>
#include <sys/ctfs.h>
#include <sys/fork.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <exception>

#include <node.h>
#include <v8.h>

#define MODE_R 0
#define MODE_W 1
#define MODE_A 2

static const int BUF_SZ = 27;
static const char *PREFIX = "%s GMT T(%d) %s: ";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Node Macros require these
using v8::Persistent;
using v8::String;

#define RETURN_EXCEPTION(MSG)                                           \
  return v8::ThrowException(v8::Exception::Error(v8::String::New(MSG)))

#define RETURN_ARGS_EXCEPTION(MSG)                                      \
  return v8::ThrowException(v8::Exception::TypeError(v8::String::New(MSG)))

#define RETURN_ERRNO_EXCEPTION(MSG)             \
  return v8::ThrowException(node::ErrnoException(errno, MSG));

#define REQUIRE_ARGS(ARGS)                      \
  if (ARGS.Length() == 0)                       \
    RETURN_ARGS_EXCEPTION("missing arguments");

#define REQUIRE_INT_ARG(ARGS, I, VAR)                                   \
  REQUIRE_ARGS(ARGS);                                                   \
  if (ARGS.Length() <= (I) || !ARGS[I]->IsNumber())                     \
    RETURN_ARGS_EXCEPTION("argument " #I " must be an integer");        \
  v8::Local<v8::Integer> _ ## VAR(ARGS[I]->ToInteger());                \
  int VAR = _ ## VAR->Value();

#define REQUIRE_STRING_ARG(ARGS, I, VAR)                        \
  REQUIRE_ARGS(ARGS);                                           \
  if (ARGS.Length() <= (I) || !ARGS[I]->IsString())             \
    RETURN_ARGS_EXCEPTION("argument " #I " must be a string");  \
  v8::String::Utf8Value VAR(ARGS[I]->ToString());

#define REQUIRE_FUNCTION_ARG(ARGS, I, VAR)                              \
  REQUIRE_ARGS(ARGS);                                                   \
  if (ARGS.Length() <= (I) || !ARGS[I]->IsFunction())                   \
    RETURN_EXCEPTION("argument " #I " must be a function");             \
  v8::Local<v8::Function> VAR = v8::Local<v8::Function>::Cast(ARGS[I]);

class eio_baton_t {
    public:
        eio_baton_t(): _path(NULL),
        _syscall(NULL),
        _zone(NULL),
        _mode(0),
        _errno(0),
        _fd(-1) {}

        virtual ~eio_baton_t() {
            _callback.Dispose();

            if (_zone != NULL) free(_zone);
            if (_path != NULL) free(_path);
            if (_syscall != NULL) free(_syscall);

            _zone = NULL;
            _path = NULL;
            _syscall = NULL;

            _fd = -1;
        }

        void setErrno(const char *syscall, int errorno) {
            if (_syscall != NULL) {
                free(_syscall);
            }
            _syscall = strdup(syscall);
            _fd = -1;
            _errno = errorno;
        }

        char *_path;
        char *_syscall;
        char *_zone;
        int _mode;
        int _errno;
        int _fd;

        v8::Persistent<v8::Function> _callback;

    private:
        eio_baton_t(const eio_baton_t &);
        eio_baton_t &operator=(const eio_baton_t &);
};


static void chomp(char *s) {
    while (*s && *s != '\n' && *s != '\r')
        s++;
    *s = 0;
}


static void debug(const char *fmt, ...) {
    char *buf = NULL;
    struct tm tm;
    time_t now;
    va_list alist;

    if (getenv("ZFILE_DEBUG") == NULL) return;

    if ((buf = (char *)alloca(BUF_SZ)) == NULL)
        return;

    now = time(0);
    gmtime_r(&now, &tm);
    asctime_r(&tm, buf);
    chomp(buf);

    va_start(alist, fmt);

    fprintf(stderr, PREFIX, buf, pthread_self(), "DEBUG");
    vfprintf(stderr, fmt, alist);
    va_end(alist);
}


static int init_template(void) {
    int fd = 0;
    int err = 0;

    fd = open64(CTFS_ROOT "/process/template", O_RDWR);
    if (fd == -1)
        return (-1);

    err |= ct_tmpl_set_critical(fd, 0);
    err |= ct_tmpl_set_informative(fd, 0);
    err |= ct_pr_tmpl_set_fatal(fd, CT_PR_EV_HWERR);
    err |= ct_pr_tmpl_set_param(fd, CT_PR_PGRPONLY | CT_PR_REGENT);
    if (err || ct_tmpl_activate(fd)) {
        (void) close(fd);
        return (-1);
    }

    return (fd);
}


static int contract_latest(ctid_t *id) {
  int cfd = 0;
  int r = 0;
  ct_stathdl_t st = {0};
  ctid_t result = {0};

  if ((cfd = open64(CTFS_ROOT "/process/latest", O_RDONLY)) == -1)
    return (errno);
  if ((r = ct_status_read(cfd, CTD_COMMON, &st)) != 0) {
    (void) close(cfd);
    return (r);
  }

  result = ct_status_get_id(st);
  ct_status_free(st);
  (void) close(cfd);

  *id = result;
  return (0);
}


static int close_on_exec(int fd) {
  int flags = fcntl(fd, F_GETFD, 0);
  if ((flags != -1) && (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1))
    return (0);
  return (-1);
}


static int contract_open(ctid_t ctid,
                         const char *type,
                         const char *file,
                         int oflag) {
  char path[PATH_MAX];
  unsigned int n = 0;
  int fd = 0;

  if (type == NULL)
    type = "all";

  n = snprintf(path, PATH_MAX, CTFS_ROOT "/%s/%ld/%s", type, ctid, file);
  if (n >= sizeof(path)) {
    errno = ENAMETOOLONG;
    return (-1);
  }

  fd = open64(path, oflag);
  if (fd != -1) {
    if (close_on_exec(fd) == -1) {
      int err = errno;
      (void) close(fd);
      errno = err;
      return (-1);
    }
  }
  return (fd);
}


static int contract_abandon_id(ctid_t ctid) {
  int fd = 0;
  int err = 0;

  fd = contract_open(ctid, "all", "ctl", O_WRONLY);
  if (fd == -1)
    return (errno);

  err = ct_ctl_abandon(fd);
  (void) close(fd);

  return (err);
}


static ssize_t read_fd(int fd, void *ptr, size_t nbytes, int *recvfd) {
  struct msghdr msg;
  struct iovec iov[1];
  ssize_t n = -1;
  union {
    struct cmsghdr cm;
    char control[CMSG_SPACE(sizeof(int))];
  } control_un;
  struct cmsghdr *cmptr = NULL;

  msg.msg_control = control_un.control;
  msg.msg_controllen = sizeof(control_un.control);
  msg.msg_name = NULL;
  msg.msg_namelen = 0;

  iov[0].iov_base = ptr;
  iov[0].iov_len = nbytes;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;

  if ((n = recvmsg(fd, &msg, 0)) <= 0) {
    return (n);
  }

  if ((cmptr = CMSG_FIRSTHDR(&msg)) != NULL &&
      cmptr->cmsg_len == CMSG_LEN(sizeof(int))) {
    if (cmptr->cmsg_level != SOL_SOCKET ||
        cmptr->cmsg_type != SCM_RIGHTS) {
      *recvfd = -1;
      errno = EINVAL;
      return (-1);
    }

    *recvfd = *(reinterpret_cast<int *>(CMSG_DATA(cmptr)));
  } else {
    *recvfd = -1;
  }

  return (n);
}


static ssize_t write_fd(int fd, void *ptr, size_t nbytes, int sendfd) {
  struct msghdr msg;
  struct iovec iov[1];
  union {
    struct cmsghdr cm;
    char control[CMSG_SPACE(sizeof(int))];
  } control_un;
  struct cmsghdr *cmptr = NULL;

  msg.msg_control = control_un.control;
  msg.msg_controllen = sizeof(control_un.control);

  cmptr = CMSG_FIRSTHDR(&msg);
  cmptr->cmsg_len = CMSG_LEN(sizeof(int));
  cmptr->cmsg_level = SOL_SOCKET;
  cmptr->cmsg_type = SCM_RIGHTS;
  *(reinterpret_cast<int *>(CMSG_DATA(cmptr))) = sendfd;

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  iov[0].iov_base = ptr;
  iov[0].iov_len = nbytes;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;

  return (sendmsg(fd, &msg, 0));
}


static int zfile(zoneid_t zoneid, const char *path, int mode) {
  char c = 0;
  ctid_t ct = -1;
  int _errno = 0;
  int pid = 0;

  /* The FD for the file we will open */
  int file_fd = 0;

  int sockfd[2] = {0};
  int stat = 0;
  int tmpl_fd = 0;
  int flags;
  int openmode = 0;

  if (zoneid < 0) {
    return (-1);
  }

  if (path == NULL) {
    return (-1);
  }

  pthread_mutex_lock(&lock);

  if ((tmpl_fd = init_template()) < 0) {
    pthread_mutex_unlock(&lock);
    return (-1);
  }

  if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sockfd) != 0) {
    (void) ct_tmpl_clear(tmpl_fd);
    pthread_mutex_unlock(&lock);
    return (-1);
  }

  pid = fork();
  debug("fork returned: %d\n", pid);
  if (pid < 0) {
    _errno = errno;
    (void) ct_tmpl_clear(tmpl_fd);
    close(sockfd[0]);
    close(sockfd[1]);
    errno = _errno;
    pthread_mutex_unlock(&lock);
    return (-1);
  }

  if (pid == 0) {
    (void) ct_tmpl_clear(tmpl_fd);
    (void) close(tmpl_fd);
    (void) close(sockfd[0]);
    int ret;

    if ((ret = zone_enter(zoneid)) != 0) {
      debug("CHILD: zone_enter(%d) => %s (%d)\n", zoneid, strerror(errno), ret);
      if (errno == EINVAL) {
        _exit(0);
      }
      _exit(1);
    }

    debug("CHILD: zone_enter(%d) => %d\n", zoneid, 0);

    switch (mode) {
        case MODE_R:
            openmode = O_RDONLY;
            break;
        case MODE_W:
            openmode = O_WRONLY | O_CREAT | O_TRUNC;
            break;
        case MODE_A:
            openmode = O_APPEND | O_CREAT;
            break;
        default:
            debug("CHILD: invalid open mode (%d)\n", mode);
            _exit(6);
    }

    if ((file_fd = open(path, openmode)) < 0) {
      debug("CHILD: open => %d\n", errno);
      _exit(2);
    }

    if (write_fd(sockfd[1], (void *)"", 1, file_fd) < 0) {
      debug("CHILD: write_fd => %d\n", errno);
      _exit(4);
    }

    debug("CHILD: write_fd => %d\n", errno);
    _exit(0);
  }

  if (contract_latest(&ct) == -1) {
    ct = -1;
  }
  (void) ct_tmpl_clear(tmpl_fd);
  (void) close(tmpl_fd);
  (void) contract_abandon_id(ct);
  (void) close(sockfd[1]);
  debug("PARENT: waitforpid(%d)\n", pid);
  while ((waitpid(pid, &stat, 0) != pid) && errno != ECHILD) ;

  if (WIFEXITED(stat) == 0) {
    debug("PARENT: Child didn't exit\n");
    _errno = ECHILD;
    file_fd = -1;
  } else {
    stat = WEXITSTATUS(stat);
    debug("PARENT: Child exit status %d\n", stat);
    if (stat == 0) {
      read_fd(sockfd[0], &c, 1, &file_fd);
    } else {
      _errno = stat;
      file_fd = -1;
    }
  }

  close(sockfd[0]);
  pthread_mutex_unlock(&lock);
  if (file_fd < 0) {
    errno = _errno;
  } else {
    if ((flags = fcntl(file_fd, F_GETFD)) != -1) {
      flags |= FD_CLOEXEC;
      (void) fcntl(file_fd, F_SETFD, flags);
    }

    errno = 0;
  }
  debug("zfile returning fd=%d, errno=%d\n", file_fd, errno);
  return (file_fd);
}


static void uv_ZFile(uv_work_t *req) {
    eio_baton_t *baton = static_cast<eio_baton_t *>(req->data);

    zoneid_t zoneid = getzoneidbyname(baton->_zone);
    if (zoneid < 0) {
        baton->setErrno("getzoneidbyname", errno);
        return;
    }
    int file_fd = -1;
    int attempts = 1;
    do {
        // This call suffers from EINTR, so just retry
        file_fd = zfile(zoneid, baton->_path, baton->_mode);
    } while (attempts++ < 3 && file_fd < 0);
    if (file_fd < 0) {
        baton->setErrno("zfile", errno);
        return;
    }

    baton->_fd = file_fd;

    return;
}


static void uv_After(uv_work_t *req, int status) {
    v8::HandleScope scope;
    eio_baton_t *baton = static_cast<eio_baton_t *>(req->data);
    delete (req);

    int argc = 1;
    v8::Local<v8::Value> argv[2];

    if (baton->_fd < 0) {
        argv[0] = node::ErrnoException(baton->_errno, baton->_syscall);
    } else {
        argc = 2;
        argv[0] = v8::Local<v8::Value>::New(v8::Null());
        argv[1] = v8::Integer::New(baton->_fd);
    }

    v8::TryCatch try_catch;

    baton->_callback->Call(v8::Context::GetCurrent()->Global(), argc, argv);

    if (try_catch.HasCaught()) {
        node::FatalException(try_catch);
    }

    delete baton;
}


static v8::Handle<v8::Value> ZFile(const v8::Arguments& args) {
    v8::HandleScope scope;

    REQUIRE_STRING_ARG(args, 0, zone);
    REQUIRE_STRING_ARG(args, 1, path);
    REQUIRE_INT_ARG(args, 2, mode);
    REQUIRE_FUNCTION_ARG(args, 3, callback);

    eio_baton_t *baton = new eio_baton_t();
    baton->_zone = strdup(*zone);
    if (baton->_zone == NULL) {
        delete baton;
        RETURN_EXCEPTION("OutOfMemory");
    }

    baton->_path = strdup(*path);
    baton->_mode = mode;
    if (baton->_path == NULL) {
        delete baton;
        RETURN_EXCEPTION("OutOfMemory");
    }

    baton->_callback = v8::Persistent<v8::Function>::New(callback);

    uv_work_t *req = new uv_work_t;
    req->data = baton;
    uv_queue_work(uv_default_loop(), req, uv_ZFile, uv_After);

    return v8::Undefined();
}


// extern "C" {
//     void init(v8::Handle<v8::Object> target) {
//         v8::HandleScope scope;
//         NODE_SET_METHOD(target, "zfile", ZFile);
//     }
// }

void Init(v8::Handle<v8::Object> exports, v8::Handle<v8::Object> module) {
//       module->Set(v8::String::NewSymbol("exports"),
//                     v8::FunctionTemplate::New(ZFile)->GetFunction());
      exports->Set(v8::String::NewSymbol("zfile"),
                    v8::FunctionTemplate::New(ZFile)->GetFunction());
}

NODE_MODULE(zfile, Init)
