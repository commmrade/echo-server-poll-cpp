#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <utility>
#include <vector>
#include <sys/ioctl.h>
#include <unordered_map>
#include <fcntl.h>

enum class ClientState {
    OUT,
    IN
};

struct Client {
    int fd;
    ClientState state;
    std::string buffer{};
    int write_bytes_total{0};
};


int main() {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        throw std::runtime_error("Could not create a socket");
    }

    int status = fcntl(server, F_SETFL, fcntl(server, F_GETFL, 0) | O_NONBLOCK);
    if (status == -1){
        perror("calling fcntl"); // Set socket to non-blocking mode
    }
        

    const int enable = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        throw std::runtime_error("Could not set socket options");
    } // So you can restart fast even when token is "in use"

    sockaddr_in addr{0};
    addr.sin_family = AF_INET; 
    addr.sin_port = htons(6969);
    addr.sin_addr.s_addr = INADDR_ANY;


    if (bind(server, (sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("Could not bind");
    } 

    if (listen(server, SOMAXCONN) < 0) {
        throw std::runtime_error("Can't start listening");
    }

    std::vector<pollfd> polls; // Tracking polls for poll()
    std::unordered_map<int, Client> clients; // Tracking clients for state
    polls.emplace_back(server, POLLIN, 0);

    while (true) {
        int poll_result = poll(polls.data(), polls.size(), -1);
        if (poll_result < 0) { // Means fatal error
            throw std::runtime_error("Poll failed");
        }
        for (auto i = 0; i < polls.size(); ++i) {
            if (polls[i].revents) { // If there is some event
                if (polls[i].fd == server) {
                    int client_socket = accept(server, nullptr, nullptr);

                    int status = fcntl(client_socket, F_SETFL, fcntl(client_socket, F_GETFL, 0) | O_NONBLOCK);
                    if (status == -1){
                        perror("calling fcntl");
                    } // Set client to non-blocking mode
                    clients[client_socket] = Client {
                        .fd = client_socket,
                        .state = ClientState::OUT,
                    };
                    polls.emplace_back(client_socket, POLLIN, 0);
                } else {
                    auto& client = clients[polls[i].fd];

                    auto remove_client = [&clients, &polls](int idx, int fd) {
                        std::swap(polls.back(), *(polls.begin() + idx));
                        clients.erase(polls[idx].fd);
                        polls.erase(polls.end());
                        close(polls[idx].fd);
                    };

                    switch (client.state) {
                        case ClientState::IN: {
                            auto wr_bytes = write(client.fd, client.buffer.data() + client.write_bytes_total, client.buffer.size() - client.write_bytes_total);

                            if (wr_bytes < 0) {
                                --i;
                                clients.erase(polls[i].fd);
                                polls.erase(polls.begin() + i);
                                close(polls[i].fd);
                                throw std::runtime_error("Could not write");
                            }
                            client.write_bytes_total += wr_bytes;
                            if (client.write_bytes_total >= client.buffer.size()) {
                                client.buffer.clear();
                                client.write_bytes_total = 0;
                                client.state = ClientState::OUT;
                                polls[i].events = POLLIN;
                                continue;
                            }
                            break;
                        }
                        case ClientState::OUT: {
                            char buffer[1024];
                            ssize_t rd_bytes = read(client.fd, buffer, sizeof(buffer));

                            if (rd_bytes < 0) {
                                std::cout << "End1\n";
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                } else { // Some weird fatal error for client
                                    --i;
                                    remove_client(i, client.fd);
                                    throw std::runtime_error("Could not read");
                                }
                            } 
                            else if (rd_bytes == 0) { // EOF which happens when client disconnects
                                remove_client(i, client.fd);
                                --i;
                                continue;
                            }

                            client.buffer.append(buffer, rd_bytes);
                            client.state = ClientState::IN;
                            polls[i].events = POLLOUT; // Switch state for writing
                            break;
                        }
                    }
                }

            } 
        }

    }


    close(server);
    return 0;
}