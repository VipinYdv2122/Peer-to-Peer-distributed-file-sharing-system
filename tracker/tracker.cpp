#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <mutex>
#include <algorithm>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <signal.h>
#include <openssl/sha.h>
#include <iomanip>
#include <cstring>
using namespace std;
#define LOG(msg) do { lock_guard<mutex> lock(cout_mutex); cout << "[LOG] " << msg << endl; } while(0)


struct User {
    string password;
    bool loggedIn = false;
    string listening_ip_port; 
};

struct Group {
    string owner;
    set<string> members;
    set<string> pendingRequests;
    vector<string> joinOrder;
};

struct FileInfo {
    string owner_username;
    string file_sha1;
    long long size = 0;
    string piece_hashes_str;
    set<string> seeders;
};

map<string, User> users;
map<string, Group> groups;
map<int, string> active_sessions; 
map<string, FileInfo> files;

mutex data_mutex;
mutex cout_mutex;

int peer_socket = -1;
mutex peer_mutex;


void sync_to_peer(const string& command);
void handle_client(int client_socket);
string process_command(const string& command, int client_socket);
void apply_sync_command(const string& sync_command);


void sync_to_peer(const string& command) {
    lock_guard<mutex> lock(peer_mutex);
    if (peer_socket != -1) {
        string sync_message = "SYNC " + command + "\n";
        if (send(peer_socket, sync_message.c_str(), sync_message.length(), 0) <= 0) {
            LOG("Peer tracker disconnected.");
            close(peer_socket);
            peer_socket = -1;
        }
    }
}

void apply_sync_command(const string& sync_command) {
    stringstream ss(sync_command);
    string action;
    ss >> action;

    lock_guard<mutex> lock(data_mutex);
    
    if (action == "CREATE_USER") {
        string username, password;
        ss >> username >> password;
        users[username] = {password, false, ""};
    } else if (action == "LOGIN") {
        string username, ip_port;
        ss >> username >> ip_port;
        users[username].loggedIn = true;
        users[username].listening_ip_port = ip_port;
    } else if (action == "LOGOUT") {
        string username;
        ss >> username;
        if(users.count(username)) users[username].loggedIn = false;
    } else if (action == "CREATE_GROUP") {
        string group_id, owner;
        ss >> group_id >> owner;
        groups[group_id].owner = owner;
        groups[group_id].members.insert(owner);
        groups[group_id].joinOrder.push_back(owner);
    } else if (action == "JOIN_GROUP") {
        string group_id, username;
        ss >> group_id >> username;
        if(groups.count(group_id)) groups[group_id].pendingRequests.insert(username);
    } else if (action == "ACCEPT_REQUEST") {
        string group_id, username;
        ss >> group_id >> username;
        if(groups.count(group_id)) {
            groups[group_id].pendingRequests.erase(username);
            groups[group_id].members.insert(username);
            groups[group_id].joinOrder.push_back(username);
        }
    } else if (action == "UPLOAD_FILE") {
        string group_id, filename, file_sha, uploader_ip;
        long long size;
        ss >> group_id >> filename >> size >> file_sha >> uploader_ip;
        string piece_hash, all_hashes;
        while(ss >> piece_hash) all_hashes += piece_hash + " ";

        string file_key = group_id + ":" + filename;
        files[file_key].size = size;
        files[file_key].file_sha1 = file_sha;
        files[file_key].seeders.insert(uploader_ip);
        files[file_key].piece_hashes_str = all_hashes;
    }
}


string process_command(const string& command, int client_socket) {
    stringstream ss(command);
    string action;
    ss >> action;

    lock_guard<mutex> lock(data_mutex);

    if (action == "create_user") {
        string username, password;
        ss >> username >> password;
        if (users.count(username)) return "ERROR User already exists.";
        users[username] = {password, false, ""};
        sync_to_peer("CREATE_USER " + username + " " + password);
        return "SUCCESS User created.";
    }

    if (action == "login") {
        string username, password, listening_ip_port;
        ss >> username >> password >> listening_ip_port;
        
        
        if (!users.count(username) || users[username].password != password) {
            return "ERROR Invalid credentials.";
        }

    
        for (auto it = active_sessions.begin(); it != active_sessions.end(); ) {
            if (it->second == username) {
                it = active_sessions.erase(it); 
            } else {
                ++it;
            }
        }
        
        
        users[username].loggedIn = true;
        users[username].listening_ip_port = listening_ip_port;
        active_sessions[client_socket] = username;

     
        sync_to_peer("LOGIN " + username + " " + listening_ip_port);
        
        return "SUCCESS Login successful.";
    }

    if (!active_sessions.count(client_socket)) {
        return "ERROR You are not logged in.";
    }
    string current_user = active_sessions[client_socket];

    if (action == "create_group") {
        string group_id;
        ss >> group_id;
        if (groups.count(group_id)) return "ERROR Group already exists.";
        groups[group_id].owner = current_user;
        groups[group_id].members.insert(current_user);
        groups[group_id].joinOrder.push_back(current_user);
        sync_to_peer("CREATE_GROUP " + group_id + " " + current_user);
        return "SUCCESS Group created.";
    }

    if (action == "join_group") {
        string group_id;
        ss >> group_id;
        if (!groups.count(group_id)) return "ERROR Group does not exist.";
        groups[group_id].pendingRequests.insert(current_user);
        sync_to_peer("JOIN_GROUP " + group_id + " " + current_user);
        return "SUCCESS Join request sent.";
    }
    
    if (action == "list_requests") {
        string group_id;
        ss >> group_id;
        if (!groups.count(group_id)) return "ERROR Group does not exist.";
        if (groups[group_id].owner != current_user) return "ERROR You are not the owner.";
        string response = "SUCCESS\n";
        for (const auto& user : groups[group_id].pendingRequests) response += user + "\n";
        return response;
    }

    if (action == "accept_request") {
        string group_id, user_to_add;
        ss >> group_id >> user_to_add;
        if (!groups.count(group_id) || groups[group_id].owner != current_user) return "ERROR Not owner or group does not exist.";
        if (!groups[group_id].pendingRequests.count(user_to_add)) return "ERROR No pending request from this user.";
        groups[group_id].pendingRequests.erase(user_to_add);
        groups[group_id].members.insert(user_to_add);
        groups[group_id].joinOrder.push_back(user_to_add);
        sync_to_peer("ACCEPT_REQUEST " + group_id + " " + user_to_add);
        return "SUCCESS User added to group.";
    }

    if (action == "list_groups") {
        string response = "SUCCESS\n";
        for (const auto& pair : groups) response += pair.first + "\n";
        return response;
    }

    if (action == "upload_file") {
        string group_id, filename, file_sha;
        long long size;
        ss >> group_id >> filename >> size >> file_sha;
        string piece_hash, all_hashes;
        while (ss >> piece_hash) all_hashes += piece_hash + " ";

        if (!groups.count(group_id) || !groups[group_id].members.count(current_user)) return "ERROR Not a member of this group.";
        
        string file_key = group_id + ":" + filename;
        if (files.count(file_key)) return "ERROR File already exists in this group.";

        files[file_key].owner_username = current_user;
        files[file_key].size = size;
        files[file_key].file_sha1 = file_sha;
        files[file_key].seeders.insert(users[current_user].listening_ip_port);
        files[file_key].piece_hashes_str = all_hashes;

        LOG("File " + filename + " uploaded to group " + group_id + " by " + current_user);
        sync_to_peer("UPLOAD_FILE " + group_id + " " + filename + " " + to_string(size) + " " + file_sha + " " + users[current_user].listening_ip_port + " " + all_hashes);
        return "SUCCESS File uploaded successfully.";
    }
    
    if (action == "list_files") {
        string group_id;
        ss >> group_id;
        if (!groups.count(group_id)) return "ERROR Group does not exist.";
        string response = "SUCCESS\n";
        for(auto const& [key, val] : files) {
            if (key.rfind(group_id + ":", 0) == 0) {
                response += key.substr(key.find(':') + 1) + "\n";
            }
        }
        return response;
    }

    if (action == "download_file") {
        string group_id, filename;
        ss >> group_id >> filename;
        string file_key = group_id + ":" + filename;

        if (!groups.count(group_id) || !groups[group_id].members.count(current_user)) return "ERROR Not a member of this group.";
        if (!files.count(file_key)) return "ERROR File does not exist in this group.";
        
        FileInfo& info = files[file_key];
        string response = "SUCCESS " + to_string(info.size) + " " + info.file_sha1 + " ";
        for (const auto& seeder : info.seeders) response += seeder + " ";
        response += "_END_SEEDERS_ ";
        response += info.piece_hashes_str;
        return response;
    }
    
    if (action == "logout") {
        users[current_user].loggedIn = false;
        users[current_user].listening_ip_port = "";
        active_sessions.erase(client_socket);
        sync_to_peer("LOGOUT " + current_user);
        return "SUCCESS Logged out.";
    }

    return "ERROR Unknown command.";
}

// --- Main Server Logic ---
void handle_client(int client_socket) {
    LOG("New client connection from " + to_string(client_socket));
    char buffer[8192];
    
    while (true) {
        memset(buffer, 0, 8192);
        int bytes_received = recv(client_socket, buffer, 8192, 0);
        if (bytes_received <= 0) {
            lock_guard<mutex> lock(data_mutex);
            if (active_sessions.count(client_socket)) {
                string user = active_sessions[client_socket];
                users[user].loggedIn = false;
                active_sessions.erase(client_socket);
                LOG(user + " disconnected.");
                sync_to_peer("LOGOUT " + user);
            }
            break;
        }
        
        string command(buffer, bytes_received);
        command.erase(remove(command.begin(), command.end(), '\n'), command.end());

        if (command.rfind("SYNC ", 0) == 0) {
            apply_sync_command(command.substr(5));
            continue;
        }
        
        string response = process_command(command, client_socket) + "\n";
        send(client_socket, response.c_str(), response.length(), 0);
    }
    close(client_socket);
}

void connect_to_peer_tracker(const string& peer_ip, int peer_port) {
    while (true) {
        {
            lock_guard<mutex> lock(peer_mutex);
            if (peer_socket == -1) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0) {
                    sockaddr_in peer_addr{};
                    peer_addr.sin_family = AF_INET;
                    peer_addr.sin_port = htons(peer_port);
                    inet_pton(AF_INET, peer_ip.c_str(), &peer_addr.sin_addr);
                    if (connect(sock, (sockaddr*)&peer_addr, sizeof(peer_addr)) == 0) {
                        peer_socket = sock;
                        LOG("Connected to other tracker for synchronization.");
                    } else {
                        close(sock);
                    }
                }
            }
        }
        this_thread::sleep_for(chrono::seconds(5));
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage: ./tracker <tracker_info.txt> <tracker_no>" << endl;
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    int tracker_info_fd = open(argv[1], O_RDONLY);
    if (tracker_info_fd < 0) { perror("open tracker_info.txt"); return 1; }
    char buffer[128];
    ssize_t n = read(tracker_info_fd, buffer, sizeof(buffer) - 1);
    if(n > 0) buffer[n] = '\0';
    close(tracker_info_fd);
    
    stringstream ss(buffer);
    string ip1, ip2;
    int port1, port2;
    ss >> ip1 >> port1 >> ip2 >> port2;

    int tracker_num = stoi(argv[2]);
    string my_ip = (tracker_num == 1) ? ip1 : ip2;
    int my_port = (tracker_num == 1) ? port1 : port2;
    string peer_ip = (tracker_num == 1) ? ip2 : ip1;
    int peer_port = (tracker_num == 1) ? port2 : port1;

    LOG("Tracker " + to_string(tracker_num) + " starting at " + my_ip + ":" + to_string(my_port));
    LOG("Other tracker at " + peer_ip + ":" + to_string(peer_port));

    thread(connect_to_peer_tracker, peer_ip, peer_port).detach();

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(my_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        return 1;
    }
    listen(server_socket, 10);
    LOG("Tracker listening for clients on port " + to_string(my_port));

    while (true) {
        int client_socket = accept(server_socket, nullptr, nullptr);
        if (client_socket < 0) {
            perror("accept failed");
            continue;
        }
        thread(handle_client, client_socket).detach();
    }

    close(server_socket);
    return 0;
}