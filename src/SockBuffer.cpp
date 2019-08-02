//
// Created by Lumin Shi on 2019-07-29.
//

#include <iostream>
#include "SockBuffer.h"

using namespace std;

#define CLIENT_WRITE_BUFFER_BLOCK_SIZE    8192        // Number of bytes to write to BMP reader from buffer

SockBuffer::SockBuffer() {
    config = Config::get_config();
    logger = Logger::get_logger();

    // init ringbuffer related variables
    ring_buffer_size = config->bmp_ring_buffer_size;
    ring_buffer = new unsigned char[ring_buffer_size];
    sock_buf_read_ptr = ring_buffer;
    sock_buf_write_ptr = ring_buffer;

    debug = (config->debug_all | config->debug_worker);
}

void SockBuffer::start(int obmp_server_sock, bool is_ipv4_connection) {
    // change running status to true
    running = true;

    // establish connection to bmp router
    // it initializes router_tcp_fd
    establish_router_connection(obmp_server_sock, is_ipv4_connection);

    // prepare router's pollfd
    pfd_tcp.fd = router_tcp_fd;
    pfd_tcp.events = POLLIN | POLLHUP | POLLERR;
    pfd_tcp.revents = 0;

    // creates a pipe sock (PF_LOCAL) between the worker and datastore
    socketpair(PF_LOCAL, SOCK_STREAM, 0, worker_to_data_store_sock_pair_fd);
    // worker reads bmp data via reader_fd; worker can get this fd via get_sockbuff_read_sock()
    reader_fd = worker_to_data_store_sock_pair_fd[0];
    // sockbuffer pushes bmp data via writer_fd
    writer_fd = worker_to_data_store_sock_pair_fd[1];
    // prepare sockbuffer's pollfd
    pfd_pipe.fd = writer_fd;
    pfd_pipe.events = POLLIN | POLLHUP | POLLERR;
    pfd_pipe.revents = 0;

    // create buffering thread
    buffer_thread = thread(&SockBuffer::sock_bufferer, this);
}

void SockBuffer::stop() {
    running = false;
    buffer_thread.join();
    if (debug) {
        LOG_INFO("sockbuffer stopped.");
    }
}

void SockBuffer::save_data() {
    if ((wrap_state and (write_position + 1) < read_position) or
        (not wrap_state and write_position < ring_buffer_size)) {

        // Attempt to read from socket
        if (poll(&pfd_tcp, 1, 5)) {
            if (pfd_tcp.revents & POLLHUP or pfd_tcp.revents & POLLERR) {
                bytes_read = 0;  // Indicate to close the connection
            } else {
                if (not wrap_state)  // write is ahead of read in terms of buffer pointer
                    bytes_read = read(router_tcp_fd, sock_buf_write_ptr,
                                      ring_buffer_size - write_position);

                else if (read_position > write_position) // read is ahead of write in terms of buffer pointer
                    bytes_read = read(router_tcp_fd, sock_buf_write_ptr,
                                      read_position - write_position - 1);
            }

            if (bytes_read <= 0) {
                close(writer_fd);
                close(reader_fd);
                close(router_tcp_fd);
                throw "bad tcp connection.";
            }
            else {
                sock_buf_write_ptr += bytes_read;
                write_position += bytes_read;
            }

        }
    } else if (write_position >= ring_buffer_size) { // if reached end of buffer space
        // Reached end of buffer, wrap to start
        write_position = 0;
        sock_buf_write_ptr = ring_buffer;
        wrap_state = true;
        // LOG_INFO("write buffer wrapped");
    }

    /* FOR DEBUGGING */
    // else LOG_INFO("ring buffer stall, waiting for read to catch up.");
}

void SockBuffer::push_data() {
    if ((not wrap_state and read_position < write_position) or
        (wrap_state and read_position < ring_buffer_size)) {

        // Attempt to write buffer to bmp reader
        if (poll(&pfd_pipe, 1, 10)) {

            if (pfd_pipe.revents & POLLHUP or pfd_pipe.revents & POLLERR) {
                throw "bad pipe connection between SockBuffer and Worker.";
            }

            if (not wrap_state) // Write buffer is a head of read in terms of buffer pointer
                bytes_read = write(writer_fd, sock_buf_read_ptr,
                                   (write_position - read_position) > CLIENT_WRITE_BUFFER_BLOCK_SIZE ?
                                   CLIENT_WRITE_BUFFER_BLOCK_SIZE : (write_position - read_position));

            else // Read buffer is ahead of write in terms of buffer pointer
                bytes_read = write(writer_fd, sock_buf_read_ptr,
                                   (ring_buffer_size - read_position) > CLIENT_WRITE_BUFFER_BLOCK_SIZE ?
                                   CLIENT_WRITE_BUFFER_BLOCK_SIZE : (ring_buffer_size - read_position));

            if (bytes_read > 0) {
                sock_buf_read_ptr += bytes_read;
                read_position += bytes_read;
            }
        }
    } else if (read_position >= ring_buffer_size) {
        read_position = 0;
        sock_buf_read_ptr = ring_buffer;
        wrap_state = false;
        //LOG_INFO("read buffer wrapped");
    }
}

void SockBuffer::establish_router_connection(int obmp_server_sock, bool is_ipv4_connection) {
    // accept the pending client request, or block till one exists
    socklen_t bmp_router_addr_len = sizeof(bmp_router_addr);
    router_tcp_fd = accept(obmp_server_sock, (struct sockaddr *) &bmp_router_addr, &bmp_router_addr_len);
    if (router_tcp_fd < 0) {
        std::string error = "Server accept connection: ";
        if (errno != EINTR)
            error += strerror(errno);
        else
            error += "Exiting normally per user request to stop server";

        throw error.c_str();
    }

    // populate the some human-readable ip information about the bmp router
    if (is_ipv4_connection){
        sockaddr_in *bmp_router_addr_v4 = (sockaddr_in *) &bmp_router_addr;
        inet_ntop(AF_INET, &bmp_router_addr_v4->sin_addr, router_ip, sizeof(router_ip));
        snprintf(router_port, sizeof(router_port), "%hu", ntohs(bmp_router_addr_v4->sin_port));
    } else {
        sockaddr_in6 *bmp_router_addr_v6 = (sockaddr_in6 *) &bmp_router_addr;
        inet_ntop(AF_INET6, &bmp_router_addr_v6->sin6_addr, router_ip, sizeof(router_ip));
        snprintf(router_port, sizeof(router_port), "%hu", ntohs(bmp_router_addr_v6->sin6_port));
    }

    if (debug) cout << "bmp router ip: " << router_ip << " port: " << router_port << endl;

    // enable TCP keepalive
    int on = 1;
    if (setsockopt(router_tcp_fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0) {
        LOG_NOTICE("%s: sock=%d: Unable to enable tcp keepalive");
    }
}

void SockBuffer::sock_bufferer() {
    LOG_INFO("SockBuffer's ringbuffer size: [%d].", ring_buffer_size);

    while (running) {
        try {
            save_data();
            push_data();
        } catch (...) {
            LOG_INFO("%s: Thread for sock [%d] ended abnormally: ", router_ip, router_tcp_fd);
            // set running to false to exit the while loop.
            running = false;
        }
        sleep(1);
    }

    if (router_tcp_fd) {
        LOG_INFO("shutting down bmp connection");
        shutdown(router_tcp_fd, SHUT_RDWR);
        close(router_tcp_fd);
    }

    // close local pipe sockets
    close(writer_fd);
    close(reader_fd);
}
