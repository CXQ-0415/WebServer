#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <string>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <unordered_map>
#include <sys/sendfile.h>
#include <errno.h>

#define MAX_EVENTS 1024
#define PORT 8080
#define MAX_CACHE_SIZE (1024 * 1024)  // 1MB以下走缓存

std::unordered_map<std::string, std::string> file_cache;
std::mutex cache_mutex;

// ========== 辅助函数 ==========
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string get_content_type(const std::string& path) {
    if (path.find(".html") != std::string::npos) return "text/html; charset=utf-8";
    if (path.find(".css") != std::string::npos) return "text/css";
    if (path.find(".js") != std::string::npos) return "application/javascript";
    if (path.find(".jpg") != std::string::npos || path.find(".jpeg") != std::string::npos) return "image/jpeg";
    if (path.find(".png") != std::string::npos) return "image/png";
    return "application/octet-stream";
}

// ========== 线程池类 ==========
class ThreadPool {
public:
    ThreadPool(size_t num_threads) : stop(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers) {
            worker.join();
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
};

// ========== 客户端处理（缓存 + sendfile + 极简日志）==========
void handle_client(int client_fd) {
    std::cout << "[线程 " << std::this_thread::get_id() << "] 处理 fd=" << client_fd << std::endl;

    char buffer[4096];
    int n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buffer[n] = '\0';

    std::string request(buffer);
    std::string path;
    size_t pos1 = request.find(' ');
    if (pos1 != std::string::npos) {
        size_t pos2 = request.find(' ', pos1 + 1);
        if (pos2 != std::string::npos) {
            path = request.substr(pos1 + 1, pos2 - pos1 - 1);
        }
    }
    if (path == "/") path = "/index.html";

    std::string filepath = "static" + path;

    struct stat file_stat;
    bool file_ok = (stat(filepath.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode));

    if (file_ok) {
        std::string content_type = get_content_type(filepath);
        std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: " + content_type + "\r\n"
            "Content-Length: " + std::to_string(file_stat.st_size) + "\r\n"
            "Connection: close\r\n"
            "\r\n";

        // 发送响应头
        size_t total = 0;
        const char* data = header.c_str();
        size_t rem = header.size();
        while (rem > 0) {
            ssize_t sent = write(client_fd, data + total, rem);
            if (sent == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                else { close(client_fd); return; }
            }
            total += sent;
            rem -= sent;
        }

        // 小文件：缓存
        if (file_stat.st_size < MAX_CACHE_SIZE) {
            std::string content;
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = file_cache.find(filepath);
            if (it != file_cache.end()) {
                content = it->second;
            } else {
                std::ifstream ifs(filepath, std::ios::binary);
                if (!ifs) { close(client_fd); return; }
                std::stringstream ss;
                ss << ifs.rdbuf();
                content = ss.str();
                file_cache[filepath] = content;
            }

            total = 0;
            data = content.c_str();
            rem = content.size();
            while (rem > 0) {
                ssize_t sent = write(client_fd, data + total, rem);
                if (sent == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    else break;
                }
                total += sent;
                rem -= sent;
            }
        }
        // 大文件：sendfile
        else {
            int file_fd = open(filepath.c_str(), O_RDONLY);
            if (file_fd != -1) {
                off_t off = 0;
                sendfile(client_fd, file_fd, &off, file_stat.st_size);
                close(file_fd);
            }
        }
    }
    // 404
    else {
        std::string body = "<h1>404 Not Found</h1>";
        std::string resp =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body;

        size_t total = 0;
        const char* data = resp.c_str();
        size_t rem = resp.size();
        while (rem > 0) {
            ssize_t sent = write(client_fd, data + total, rem);
            if (sent == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                else break;
            }
            total += sent;
            rem -= sent;
        }
    }

    close(client_fd);
    std::cout << "[线程 " << std::this_thread::get_id() << "] 完成 fd=" << client_fd << std::endl;
}

// ========== main ==========
int main() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == -1) {
        return 1;
    }
    listen(listen_fd, 10);
    set_nonblocking(listen_fd);

    int epoll_fd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    ThreadPool pool(8);
    std::cout << "服务器启动 端口:" << PORT << std::endl;

    epoll_event events[MAX_EVENTS];
    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds <= 0) continue;

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
                    if (client_fd == -1) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {}
                        break;
                    }
                    std::cout << "[新连接] fd=" << client_fd << std::endl;
                    pool.enqueue([client_fd] { handle_client(client_fd); });
                }
            }
        }
    }

    close(listen_fd);
    close(epoll_fd);
    return 0;
}
