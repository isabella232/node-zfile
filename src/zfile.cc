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

static const int BUF_SZ = 27;
// static const char *PREFIX = "%s GMT T(%d) %s: ";
// static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

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
        int _errno;
        int _fd;

        v8::Persistent<v8::Function> _callback;

    private:
        eio_baton_t(const eio_baton_t &);
        eio_baton_t &operator=(const eio_baton_t &);
};


static void debug(const char *fmt, ...) {
  char *buf = NULL;
  struct tm tm = {};
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


static int zfile(zoneid_t zoneid, const char *path) {
  char c = 0;
  ctid_t ct = -1;
  int _errno = 0;
  int pid = 0;
  int sock_fd = 0;
  int sockfd[2] = {0};
  int stat = 0;
  int tmpl_fd = 0;
  int flags;
  struct sockaddr_un addr = {0};
  size_t addr_len = 0;

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


// static void EIO_ZSocket(uv_work_t *req) {
//     eio_baton_t *baton = static_cast<eio_baton_t *>(req->data);
// 
//     zoneid_t zoneid = getzoneidbyname(baton->_zone);
//     if (zoneid < 0) {
//         baton->setErrno("getzoneidbyname", errno);
//         return;
//     }
//     int sock_fd = -1;
//     int attempts = 1;
//     do {
//         // This call suffers from EINTR, so just retry
//         sock_fd = zsocket(zoneid, baton->_path);
//     } while (attempts++ < 3 && sock_fd < 0);
//     if (sock_fd < 0) {
//         baton->setErrno("zsocket", errno);
//         return;
//     }
// 
//     if (listen(sock_fd, baton->_backlog) != 0) {
//         baton->setErrno("listen", errno);
//         return;
//     }
// 
//     baton->_fd = sock_fd;
// 
//     return;
// }


static void EIO_ZFile(uv_work_t *req) {
    eio_baton_t *baton = static_cast<eio_baton_t *>(req->data);

    zoneid_t zoneid = getzoneidbyname(baton->_zone);
    if (zoneid < 0) {
        baton->setErrno("getzoneidbyname", errno);
        return;
    }
    int sock_fd = 666;

    baton->_fd = sock_fd;

    return;
}


static void EIO_After(uv_work_t *req) {
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
    REQUIRE_FUNCTION_ARG(args, 2, callback);

    eio_baton_t *baton = new eio_baton_t();
    baton->_zone = strdup(*zone);
    if (baton->_zone == NULL) {
        delete baton;
        RETURN_EXCEPTION("OutOfMemory");
    }

    baton->_path = strdup(*path);
    if (baton->_path == NULL) {
        delete baton;
        RETURN_EXCEPTION("OutOfMemory");
    }

    baton->_callback = v8::Persistent<v8::Function>::New(callback);

    uv_work_t *req = new uv_work_t;
    req->data = baton;
    uv_queue_work(uv_default_loop(), req, EIO_ZFile, EIO_After);

    return v8::Undefined();
}


extern "C" {
    void init(v8::Handle<v8::Object> target) {
        v8::HandleScope scope;
        NODE_SET_METHOD(target, "zfile", ZFile);
    }
}