# C++ Epoll 高性能 Web 服务器

基于 Linux epoll + 线程池 实现的高并发静态资源 Web 服务器

## 核心特性
- 高并发 IO 多路复用：epoll ET 模式
- 线程池异步处理请求
- 小文件内存缓存，提升访问速度
- 大文件 sendfile 零拷贝传输
- 支持 HTML/CSS/JS/PNG/JPG 静态资源
- HTTP 1.1 标准响应
- 极简日志，便于调试

## 技术栈
- C++11
- epoll
- 线程池
- 文件缓存
- sendfile 零拷贝
- HTTP 协议解析

## 编译运行
```bash
g++ -o server server.cpp -pthread
./server

## 访问
http://127.0.0.1:8080
