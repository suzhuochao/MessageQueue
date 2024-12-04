/*
 * File:   connection_socket.h
 *
 *
 * Created on February 26, 2015, 7:41 AM
 */

#ifndef CONNECTION_SOCKET_H
#define CONNECTION_SOCKET_H
// #include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "log.h"
#include "connection.h"
#include "utils.h"
#include "broker_config.h"
#include "broker_storage.h"
using namespace mymq;
namespace myq
{

    class connection_socket : public connection
    {
    public:
        // read fd call back
        typedef int (*process_fd_callback)(int fd);

        // constuctor

        connection_socket(
            const std::string &topic,
            const std::string &uri,
            endpoint_type ep_type,
            connection::socket_connect_type connect_type = connection::bind_socket,
            bool non_blocking = true,
            bool client_pull = false) : connection(topic, uri, connection::stream_socket,
                                                   ep_type,
                                                   connect_type,
                                                   non_blocking),
                                        client_pull_(client_pull)
        {

            LOG_IN("topic: %s, uri: %s, endpoint_type: %d", topic.c_str(), uri.c_str(), ep_type);
            socket_ = -1;
            listen_fd_ = -1;
            stop_ = false;
            process_fd_callback_ = NULL;
            current_write_offset_ = 0;
            current_fd_index_ = 0;
            p_storage_ = 0;
            LOG_OUT("");
        }

        /**
         * destructor
         */
        ~connection_socket()
        {
            LOG_IN("");
            if (bind_thread_id_.joinable())
            {
                bind_thread_id_.join();
            }
            if (socket_ > -1)
            {
                close(socket_);
            }
            LOG_OUT("");
        }

        // init

        bool init(broker_storage *pstorage)
        {
            LOG_IN("");
            p_storage_ = pstorage;
            bool result = false;
            if (socket_connect_type_ == socket_connect_type::bind_socket)
            {
                result = init_server();
            }
            else
            {
                result = init_client();
            }
            if (result)
            {
                LOG_RET_TRUE("success");
            }
            else
            {

                LOG_RET_FALSE("Failed to initialize");
            }
        }

        bool init()
        {
            LOG_IN("");
            return init(NULL);
        }

        /**
         * init client
         * @param remote_host
         * @param port
         * @return
         */
        bool init_client()
        {
            if (!utils::convert_uri_host_port(resource_uri_, host_, port_))
            {
                LOG_ERROR("Failed to get the host and port from resource_uri_: %s", resource_uri_.c_str());
                LOG_RET_FALSE("failed");
            }
            struct sockaddr_in servaddr;
            socket_ = socket(AF_INET, SOCK_STREAM, 0);

            bzero(&servaddr, sizeof(servaddr));
            servaddr.sin_family = AF_INET;
            servaddr.sin_addr.s_addr = inet_addr(host_.c_str());
            servaddr.sin_port = htons(port_);

            if (connect(socket_, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
            {
                LOG_ERROR("Socket error while connecting to %s:%u ", host_.c_str(), port_);
                return false;
            }
            int iMode = 0;
            ioctl(socket_, FIONBIO, &iMode);
            /* if (fcntl(socket_, F_SETFL, fcntl(socket_, F_GETFL, 0) | O_NONBLOCK) == -1) {
                 LOG_ERROR("Failed : ioctl FIONBIO  on socket connected to %s:%d. Err: %d, ErrDesc: %s",
                         host_.c_str(), port_, errno, strerror(errno));
             } else {
                 LOG_DEBUG("Successfully set non-block to socket : %d", socket_);
             }*/

            std::string remote_host;
            uint32_t remote_port;
            if (get_remote_address(socket_, remote_host, remote_port))
            {
                LOG_EVENT("Socket %d is connected to host: %s:%u", socket_, remote_host.c_str(), remote_port);
            }
            LOG_RET_TRUE("successfully initialized client socket")
        }

        /**
         * initialize server
         * @return
         */
        bool init_server()
        {
            LOG_IN("");
            if (!utils::convert_uri_host_port(resource_uri_, host_, port_))
            {
                LOG_ERROR("Failed to get the host and port from resource_uri_: %s", resource_uri_.c_str());
                LOG_RET_FALSE("failed");
            }
            // utils::replace(host_, "*", "127.0.0.1");
            LOG_DEBUG("Binding to host[%s], Port[%d]", host_.c_str(), port_);

            struct sockaddr_in serv_addr;
            listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
            memset(&serv_addr, '0', sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            if (host_ == "*")
            {
                serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            }
            else
            {
                serv_addr.sin_addr.s_addr = inet_addr(host_.c_str());
            }
            int opt = true;
            // set master socket to allow multiple connections , this is just a good habit, it will work without this
            if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0)
            {
                LOG_ERROR("Failed to setsockopt for SO_REUSEADDR ");
            }
            serv_addr.sin_port = htons(port_);
            LOG_EVENT("Binding to port %u", port_);
            bind(listen_fd_, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
            listen(listen_fd_, 10);
            LOG_RET_TRUE("");
        }

        /**
         * run
         * @return
         */
        bool run()
        {
            LOG_IN("");
            bind_thread_id_ = std::thread(
                [&]
                {
                    if (endpoint_type_ == endpoint_type::conn_broker)
                    {
                        LOG_TRACE("Running run_broker_loop");
                        run_broker_loop();
                    }
                    else
                    {
                        LOG_TRACE("Running server loop");
                        while (!stop_)
                        {
                            LOG_DEBUG("Accepting connection...");
                            int connfd = accept(listen_fd_, (struct sockaddr *)NULL, NULL);
                            if (connfd < 0)
                            {
                                LOG_ERROR("Failed to accept connect on listen fd: %d", listen_fd_);
                                continue;
                            }
                            LOG_DEBUG("Received connect connection");
                            std::string remote_host;
                            uint32_t remote_port;
                            if (get_remote_address(connfd, remote_host, remote_port))
                            {
                                LOG_EVENT("Received connection from remote  host: %s:%u for fd: %d",
                                          remote_host.c_str(), remote_port, connfd);
                            }
                            else
                            {
                                LOG_ERROR("Failed to get remote address");
                            }
                            // int status = fcntl(connfd, F_SETFL, fcntl(connfd, F_GETFL, 0) | O_NONBLOCK);
                            /* if (non_blocking_ && (ioctl(connfd, (int) FIONBIO, (char *) &on) == -1)) {
                                 LOG_ERROR("Failed : ioctl FIONBIO  on socket connected to %s:%d. Err: %d, ErrDesc: %s",
                                         host_.c_str(), port_, errno, strerror(errno));
                             }*/

                            fds_.push_back(connfd);
                            LOG_EVENT("Connect with FD %d is connected. Total clients: %u", connfd, fds_.size());
                        }
                        LOG_DEBUG("Existing while loop");
                    }
                });
            LOG_RET_TRUE("");
        }

        /**
         * run broker loop
         * @return
         */
        bool run_broker_loop()
        {

            fd_set active_fd_set, read_fd_set;
            struct sockaddr_in client_addr;
            FD_ZERO(&active_fd_set);
            FD_SET(listen_fd_, &active_fd_set);
            while (!stop_)
            {
                read_fd_set = active_fd_set;
                if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL) < 0)
                {
                    LOG_ERROR("Failed to select on read fd");
                    LOG_RET_FALSE("");
                }
                for (int i = 0; i < FD_SETSIZE; ++i)
                {

                    if (FD_ISSET(i, &read_fd_set))
                    {
                        if (i == listen_fd_)
                        {
                            socklen_t slen = sizeof(client_addr);
                            int connfd = accept(listen_fd_, (struct sockaddr *)&client_addr, &slen);
                            if (connfd < 0)
                            {
                                LOG_ERROR("Failed to accept connect on listen fd: %d", listen_fd_);
                                continue;
                            }
                            FD_SET(connfd, &active_fd_set);
                            if (fcntl(socket_, F_SETFL, fcntl(socket_, F_GETFL, 0) | O_NONBLOCK) == -1)
                            {
                                LOG_ERROR("Failed : ioctl FIONBIO  on socket connected to %s:%d. Err: %d, ErrDesc: %s",
                                          host_.c_str(), port_, errno, strerror(errno));
                            }
                            else
                            {
                                LOG_DEBUG("Successfully set non-block to socket : %d", socket_);
                            }
                            std::string remote_host;
                            uint32_t remote_port;
                            if (get_remote_address(client_addr, remote_host, remote_port))
                            {
                                LOG_EVENT("Received connection from remote  host: %s:%u for fd: %d",
                                          remote_host.c_str(), remote_port, connfd);
                            }
                            // fds_.push_back(connfd);
                        }
                        else
                        {
                            if (endpoint_type_ == conn_consumer)
                            {
                                ssize_t offset = read_client_offset(i);
                                p_storage_->get_file_connection()->send_file(
                                    i, offset,
                                    p_storage_->get_file_total_bytes_written() -
                                        offset);
                            }
                            else if (endpoint_type_ == conn_publisher)
                            {
                                buffer_[0] = '\0';
                                ssize_t bytes_read = utils::read_size(i, true);
                                if (bytes_read < 0)
                                {
                                    remove_fd(i);
                                    continue;
                                }
                                else if (bytes_read == 0)
                                {
                                    continue;
                                }
                                bytes_read = utils::read_buffer(i, buffer_, utils::max_msg_size, bytes_read);
                                if (bytes_read < 0)
                                {
                                    remove_fd(i);
                                    continue;
                                }
                                if (!p_storage_->add_to_storage(buffer_, bytes_read, true))
                                {
                                    LOG_RET_FALSE("failure");
                                }
                            }
                            else
                            {
                                ssize_t result = utils::read_line(i, buffer_, utils::max_msg_size);
                                LOG_DEBUG("Read line with bytes : %d", result);
                            }
                        }
                    }
                }
            }
            LOG_RET_TRUE("loop exit");
        }

        ssize_t read_client_offset(int fd)
        {
            LOG_IN("fd[%d]", fd);
            ssize_t offset = utils::read_size(fd, true);
            LOG_RET("Offset read", offset);
        }

        /**
         * get next fd
         * @return
         */
        unsigned get_next_fd()
        {
            LOG_IN("");
            if (fds_.size() == 0)
            {
                LOG_RET("No fd", -1);
            }
            if (current_fd_index_ >= fds_.size())
            {
                current_fd_index_ = 0;
            }
            int fd = fds_[current_fd_index_++];
            LOG_DEBUG("returning fd: %d", fd);
            LOG_RET("fd: %d", fd);
        }

        std::vector<int> get_active_fds()
        {
            return fds_;
        }

        unsigned get_total_connected_clients()
        {
            return fds_.size();
        }

        /**
         * get next fd
         * @return
         */
        bool remove_fd(int fd)
        {
            LOG_IN("");
            if (fds_.size() == 0)
            {
                LOG_RET("No fd", false);
            }
            fds_.erase(std::remove(fds_.begin(), fds_.end(), fd), fds_.end());
            LOG_RET_TRUE("success");
        }

        /**
         * write
         * @param message
         * @return
         */
        ssize_t write_msg(const std::string &message)
        {
            LOG_IN("message:%s", message.c_str());
            if (endpoint_type_ != endpoint_type::conn_broker)
            {
                while (fds_.size() > 0)
                {

                    if (current_fd_index_ >= fds_.size())
                    {
                        current_fd_index_ = 0;
                    }
                    ssize_t result = utils::write_size(fds_[current_fd_index_], message.length(), true);
                    if (result == sizeof(message.length()))
                    {
                        LOG_ERROR("Failed to write payload size to socket :%d", fds_[current_fd_index_]);
                        remove_fd(current_fd_index_);
                        continue;
                    }
                    result = utils::write_buffer(fds_[current_fd_index_], message.c_str(), message.length());

                    if (result == -1)
                    {
                        LOG_ERROR("Failed to write to socket :%d", fds_[current_fd_index_]);
                        remove_fd(current_fd_index_);
                        continue;
                        // don't increment because we failed so next would be next element
                    }
                    else if (result == 0)
                    {
                        ++current_fd_index_;
                        continue;
                    }
                    else
                    {
                        ++current_fd_index_;
                        LOG_RET("success", result);
                    }
                }
                LOG_RET("no consumers", 0);
            }
            else
            {
                throw std::runtime_error("For broker connection, write must be handled in callback function");
            }
        }

        /**
         * write
         * @param message
         * @return
         */
        ssize_t write_msg(const char *message, unsigned length)
        {
            LOG_IN("message:%s", message);
            if (endpoint_type_ != endpoint_type::conn_broker)
            {
                while (fds_.size() > 0)
                {

                    if (current_fd_index_ >= fds_.size())
                    {
                        current_fd_index_ = 0;
                    }
                    ssize_t result = utils::write_size(fds_[current_fd_index_], length, true);
                    if (result == sizeof(length))
                    {
                        LOG_ERROR("Failed to write payload size to socket :%d", fds_[current_fd_index_]);
                        remove_fd(current_fd_index_);
                        continue;
                    }
                    result = utils::write_buffer(fds_[current_fd_index_], message, length);

                    if (result == -1)
                    {
                        LOG_ERROR("Failed to write to socket :%d", fds_[current_fd_index_]);
                        remove_fd(current_fd_index_);
                        continue;
                        // don't increment because we failed so next would be next element
                    }
                    else if (result == 0)
                    {
                        ++current_fd_index_;
                        continue;
                    }
                    else
                    {
                        ++current_fd_index_;
                        LOG_RET("success", result);
                    }
                }
                LOG_RET("no consumers", 0);
            }
            else
            {
                throw std::runtime_error("For broker connection, write must be handled in callback function");
            }
        }

        ssize_t client_socket_read_msg(std::string &message, bool ntohl = false)
        {
            LOG_IN("");

            LOG_DEBUG("Reading size from socket[%d]", socket_);
            ssize_t result = 0;
            while (result <= 0)
            {
                result = utils::read_size(socket_, ntohl);
                if (result < 0)
                {
                    LOG_ERROR("Failed to write payload size to socket :%d", socket_);

                    LOG_RET("error", -1);
                }
                else if (result == 0)
                {
                    LOG_DEBUG("no data available to read :%d", socket_);
                    utils::sleep_ms(utils::queue_poll_wait);
                }
                else if (result > utils::max_msg_size)
                {
                    LOG_ERROR("This should not happen. Size to read [%lld]", result);
                    result = 0;
                    utils::sleep_ms(utils::queue_poll_wait);
                }
            }
            LOG_DEBUG("Received  size  to read[ %u]", result);
            buffer_[0] = '\0';
            uint32_t size_to_read = result;
            if (size_to_read > utils::max_msg_size)
            {
                size_to_read = utils::max_msg_size;
            }

            result = 0;
            while (result <= 0)
            {
                result = utils::read_buffer(socket_, (char *)buffer_, utils::max_msg_size, size_to_read);
                if (result == -1)
                {
                    LOG_ERROR("Failed to write to socket :%d", socket_);

                    LOG_RET("error", -1);
                }
                if (result == 0)
                {
                    LOG_DEBUG("read timeout.  Trying again..");
                    utils::sleep_ms(utils::queue_poll_wait);
                }
            }
            if (result > 0)
            {
                buffer_[result] = '\0';
                message.assign(buffer_, result);
            }
            LOG_DEBUG("Received bytes [ %u]", result);
            LOG_RET("", result);
        }

        ssize_t client_socket_read_msg(char *msg_buffer, unsigned length, bool ntohl = false)
        {
            LOG_IN("");

            LOG_DEBUG("Reading size from socket[%d]", socket_);
            ssize_t result = 0;
            while (result <= 0)
            {
                result = utils::read_size(socket_, ntohl);
                if (result < 0)
                {
                    LOG_ERROR("Failed to write payload size to socket :%d", socket_);

                    LOG_RET("error", -1);
                }
                else if (result == 0)
                {
                    LOG_DEBUG("no data available to read :%d", socket_);
                }
            }
            LOG_DEBUG("Received  size  to read[ %u]", result);
            msg_buffer[0] = '\0';
            uint32_t size_to_read = result;
            result = 0;
            while (result <= 0)
            {
                result = utils::read_buffer(socket_, msg_buffer, length, size_to_read);
                if (result == -1)
                {
                    LOG_ERROR("Failed to write to socket :%d", socket_);

                    LOG_RET("error", -1);
                }
                if (result == 0)
                {
                    LOG_DEBUG("read timeout.  Trying again..");
                    utils::sleep_ms(utils::queue_poll_wait);
                }
            }
            LOG_DEBUG("Received bytes [ %u]", result);
            LOG_RET("", result);
        }

        // read

        ssize_t read_msg(std::string &message)
        {
            return read_msg(message, false);
        }

        ssize_t read_msg(char *message, uint32_t length, bool ntohl = false)
        {
            if (endpoint_type_ == endpoint_type::conn_consumer &&
                socket_connect_type_ == socket_connect_type::connect_socket)
            {
                LOG_DEBUG("Client socket type.");
                return client_socket_read_msg(message, length, ntohl);
            }
            LOG_ERROR("Not implemented");
            LOG_RET("Not implemented", -1);
        }

        ssize_t read_msg(std::string &message, bool ntohl)
        {
            LOG_IN("");
            ssize_t result = -1;

            if (endpoint_type_ == endpoint_type::conn_consumer &&
                socket_connect_type_ == socket_connect_type::connect_socket)
            {
                LOG_DEBUG("Client socket type.");
                return client_socket_read_msg(message, ntohl);
            }

            LOG_DEBUG("Number of FDS[%d]", fds_.size());
            while (fds_.size() > 0)
            {
                if (current_fd_index_ >= fds_.size())
                {
                    current_fd_index_ = 0;
                }
                LOG_DEBUG("Current fd index [%d],  fd[%d]", current_fd_index_, fds_[current_fd_index_]);
                if (endpoint_type_ != endpoint_type::conn_broker)
                {
                    LOG_DEBUG("Reading siz from fd[%d]", fds_[current_fd_index_]);
                    result = utils::read_size(fds_[current_fd_index_], ntohl);
                    if (result < 0)
                    {
                        LOG_ERROR("Failed to write payload size to socket :%d", fds_[current_fd_index_]);
                        remove_fd(current_fd_index_);
                        continue;
                    }
                    else if (result == 0)
                    {
                        LOG_DEBUG("no data available to read :%d", fds_[current_fd_index_]);
                        ++current_fd_index_;
                        continue;
                    }
                    LOG_DEBUG("Received  size  to read[ %u]", result);
                    buffer_[0] = '\0';
                    result = 0;
                    while (result <= 0)
                    {
                        result = utils::read_buffer(
                            fds_[current_fd_index_], (char *&)buffer_, utils::max_msg_size,
                            result);
                        if (result == -1)
                        {
                            LOG_ERROR("Failed to write to socket :%d", fds_[current_fd_index_]);
                            remove_fd(current_fd_index_);
                            continue;
                            // don't increment because we failed so next would be next element
                        }
                        if (result == 0)
                        {
                            LOG_DEBUG("read timeout.  Trying again..");
                            utils::sleep_ms(utils::queue_poll_wait);
                        }
                    }
                }
                else
                {
                    result = utils::read_line(fds_[current_fd_index_], buffer_, utils::max_msg_size);
                }
                if (result == -1)
                {
                    LOG_ERROR("Failed to write to socket :%d", fds_[current_fd_index_]);
                    remove_fd(current_fd_index_);
                    continue;
                    // don't increment because we failed so next would be next element
                }
                else if (result == 0)
                {
                    ++current_fd_index_;
                    LOG_DEBUG("Received zerobyte");
                    continue;
                }
                else
                {
                    ++current_fd_index_;
                    std::string s(buffer_, result);
                    message.swap(s);
                    LOG_RET("success", result);
                }
                utils::sleep_ms(utils::queue_poll_wait);
            }
            return 0;
            LOG_RET("", 0);
        }

        uint64_t get_write_offset()
        {
            //  LOG_IN("");
            //  LOG_DEBUG("current_write_offset_[%u]", current_write_offset_);
            //   LOG_RET("", current_write_offset_);
            return current_write_offset_;
        }

        void set_write_offset(uint64_t offset)
        {
            //   LOG_IN("ofset[%u]", offset);
            current_write_offset_ = offset;
            //  LOG_OUT("");
        }

        /**
         * send offset
         * @param offset
         * @return
         */
        ssize_t send_offset(uint32_t offset)
        {
            LOG_IN("");
            ssize_t bytes_written = utils::write_size(socket_, offset, true);
            LOG_RET("", bytes_written);
        }
        //

        bool is_consumer_pull_messages()
        {
            return client_pull_;
        }

    private:
        /**
         * get remote address
         * @param sock_addr
         * @param host
         * @param port
         * @return
         */
        bool get_remote_address(struct sockaddr_in &sock_addr, std::string &host, uint32_t &port)
        {
            LOG_IN("sock_addr: %p", &sock_addr);
            host = inet_ntoa(sock_addr.sin_addr);
            port = ntohs(sock_addr.sin_port);
            LOG_RET_TRUE("");
        }

        /**
         * get remote address
         * @param fd
         * @param host
         * @param port
         * @return
         */
        bool get_remote_address(int fd, std::string &host, uint32_t &port)
        {
            LOG_IN("fd: %d", fd);
            socklen_t len;
            struct sockaddr_storage addr;
            char ipstr[INET6_ADDRSTRLEN];

            len = sizeof addr;
            int err = getpeername(fd, (struct sockaddr *)&addr, &len);
            if (err == -1)
            {
                LOG_ERROR("Failed to get remote address. Err: %d,  Description: %s", err, strerror(err));
                LOG_RET_FALSE("Failed to get remote address");
            }

            // deal with both IPv4 and IPv6:
            if (addr.ss_family == AF_INET)
            {
                struct sockaddr_in *s = (struct sockaddr_in *)&addr;
                port = ntohs(s->sin_port);
                inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
            }
            else
            { // AF_INET6
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
                port = ntohs(s->sin6_port);
                inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
            }
            host = ipstr;
            LOG_DEBUG("Remote address: IP: %s,  Port: %u", host.c_str(), port);
            LOG_RET_TRUE("success");
        }

        int socket_;
        //  uint64_t total_msg_written_;
        //  uint64_t total_msg_read_;
        //  uint64_t total_bytes_written_;
        //  uint64_t total_bytes_read_;
        int listen_fd_;
        std::string host_;
        uint32_t port_;
        std::thread bind_thread_id_;
        bool stop_;
        std::vector<int> fds_;
        uint64_t current_write_offset_;
        unsigned current_fd_index_;
        process_fd_callback process_fd_callback_;
        char buffer_[utils::max_msg_size]; // 128*1024 not thread safe
        bool client_pull_;
        broker_storage *p_storage_;
    };
}
#endif /* CONNECTION_SOCKET_H */
