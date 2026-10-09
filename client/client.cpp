#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <mutex>
#include <map>
#include <queue>
#include <functional>
#include <atomic>
#include <random>
#include <algorithm>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <iomanip>
#include <cstring>
using namespace std;

#define LOG(msg) do { lock_guard<mutex> lock(cout_mutex); cout << "[LOG] " << msg << endl; } while(0)
#define PIECE_SIZE 524288


struct DownloadState {
    string group_id;
    string filename;
    int total_pieces = 0;
    atomic<int> completed_pieces{0};
    atomic<bool> is_complete{false};
};


string my_listening_address;
int tracker_socket = -1;
mutex cout_mutex;
map<string, string> uploaded_files;
mutex files_mutex;
map<string, DownloadState> g_downloads;
mutex g_downloads_mutex;
string g_tracker1_ip, g_tracker2_ip;
int g_tracker1_port, g_tracker2_port;
string g_current_user, g_current_password;
mutex connection_mutex;


bool ensure_connection();


string sha1_to_hex(const unsigned char* sha1) {
    stringstream ss;
    ss << hex << setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) ss << setw(2) << static_cast<unsigned int>(sha1[i]);
    return ss.str();
}

string get_file_sha1(const string& path) {
    int file_d = open(path.c_str(), O_RDONLY);
    if (file_d < 0) return "";
    struct stat file_stat;
    if (fstat(file_d, &file_stat) < 0) { close(file_d); return ""; }
    long long file_size = file_stat.st_size;
    if (file_size == 0) {
        close(file_d);
        unsigned char hash[SHA_DIGEST_LENGTH];
        SHA1((unsigned char*)"", 0, hash);
        return sha1_to_hex(hash);
    }
    char* buffer = new char[file_size];
    ssize_t bytes_read = read(file_d, buffer, file_size);
    close(file_d);
    if (bytes_read != file_size) { delete[] buffer; return ""; }
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)buffer, file_size, hash);
    delete[] buffer;
    return sha1_to_hex(hash);
}

string get_piece_hashes(const string& path) {
    string all_hashes;
    int file_d = open(path.c_str(), O_RDONLY);
    if (file_d < 0) return "";
    char buffer[PIECE_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(file_d, buffer, PIECE_SIZE)) > 0) {
        unsigned char hash[SHA_DIGEST_LENGTH];
        SHA1((unsigned char*)buffer, bytes_read, hash);
        all_hashes += sha1_to_hex(hash) + " ";
    }
    close(file_d);
    return all_hashes;
}

void handle_peer_request(int peer_socket) {
    char buffer[1024] = {0};
    recv(peer_socket, buffer, 1024, 0);
    stringstream ss(buffer);
    string command, filename;
    int piece_index;
    ss >> command >> filename >> piece_index;
    if (command == "GET_PIECE") {
        string local_path;
        {
            lock_guard<mutex> lock(files_mutex);
            if (uploaded_files.count(filename)) local_path = uploaded_files[filename];
        }
        if (!local_path.empty()) {
            int file_d = open(local_path.c_str(), O_RDONLY);
            if (file_d >= 0) {
                lseek(file_d, (off_t)piece_index * PIECE_SIZE, SEEK_SET);
                char piece_buffer[PIECE_SIZE];
                ssize_t bytes_read = read(file_d, piece_buffer, PIECE_SIZE);
                if (bytes_read > 0) send(peer_socket, piece_buffer, bytes_read, 0);
                close(file_d);
            }
        }
    }
    close(peer_socket);
}

void run_peer_server(int port) {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_socket, 10);
    LOG("Seeder listening on port " + to_string(port));
    while (true) {
        int peer_socket = accept(server_socket, nullptr, nullptr);
        thread(handle_peer_request, peer_socket).detach();
    }
}

bool download_piece_from_peer(const string& seeder_ip_port, const string& filename_on_seeder, int piece_index, const string& temp_path) {
    size_t colon_pos = seeder_ip_port.find(':');
    if (colon_pos == string::npos) return false;
    string seeder_ip = seeder_ip_port.substr(0, colon_pos);
    int seeder_port = stoi(seeder_ip_port.substr(colon_pos + 1));
    int peer_socket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(seeder_port);
    inet_pton(AF_INET, seeder_ip.c_str(), &peer_addr.sin_addr);
    if (connect(peer_socket, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) < 0) { close(peer_socket); return false; }
    string request = "GET_PIECE " + filename_on_seeder + " " + to_string(piece_index);
    send(peer_socket, request.c_str(), request.length(), 0);
    char buffer[PIECE_SIZE];
    ssize_t bytes_received = recv(peer_socket, buffer, PIECE_SIZE, MSG_WAITALL);
    close(peer_socket);
    if (bytes_received <= 0) return false;
    int temp_fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(temp_fd < 0) return false;
    write(temp_fd, buffer, bytes_received);
    close(temp_fd);
    return true;
}

void assemble_file(const string& dest_path, int num_pieces) {
    int dest_fd = open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(dest_fd < 0) { LOG("Failed to create final file: " + dest_path); return; }
    char buffer[PIECE_SIZE];
    for (int i = 0; i < num_pieces; ++i) {
        string temp_path = dest_path + ".part" + to_string(i);
        int temp_fd = open(temp_path.c_str(), O_RDONLY);
        if (temp_fd >= 0) {
            ssize_t bytes_read = read(temp_fd, buffer, PIECE_SIZE);
            if(bytes_read > 0) write(dest_fd, buffer, bytes_read);
            close(temp_fd);
            unlink(temp_path.c_str());
        }
    }
    close(dest_fd);
}


bool connect_to_available_tracker() {
    // Try tracker 1
    tracker_socket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in tracker_addr{};
    tracker_addr.sin_family = AF_INET;
    tracker_addr.sin_port = htons(g_tracker1_port);
    inet_pton(AF_INET, g_tracker1_ip.c_str(), &tracker_addr.sin_addr);
    if (connect(tracker_socket, (struct sockaddr*)&tracker_addr, sizeof(tracker_addr)) == 0) {
        LOG("Successfully connected to primary tracker at " + g_tracker1_ip + ":" + to_string(g_tracker1_port));
        return true;
    }
    close(tracker_socket);

    // Try tracker 2
    tracker_socket = socket(AF_INET, SOCK_STREAM, 0);
    tracker_addr.sin_port = htons(g_tracker2_port);
    inet_pton(AF_INET, g_tracker2_ip.c_str(), &tracker_addr.sin_addr);
    if (connect(tracker_socket, (struct sockaddr*)&tracker_addr, sizeof(tracker_addr)) == 0) {
        LOG("Successfully connected to secondary tracker at " + g_tracker2_ip + ":" + to_string(g_tracker2_port));
        return true;
    }
    close(tracker_socket);
    tracker_socket = -1;
    return false;
}

bool ensure_connection() {
    lock_guard<mutex> lock(connection_mutex);
    if (tracker_socket != -1) {
        char buf;
        ssize_t result = recv(tracker_socket, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (result != 0) {
            return true;
        }
        close(tracker_socket);
        tracker_socket = -1;
    }

    LOG("Connection lost. Attempting to reconnect...");
    if (!connect_to_available_tracker()) {
        LOG("No trackers available.");
        return false;
    }

    if (!g_current_user.empty()) {
        LOG("Re-logging in as " + g_current_user);
        string relogin_cmd = "login " + g_current_user + " " + g_current_password + " " + my_listening_address + "\n";
        send(tracker_socket, relogin_cmd.c_str(), relogin_cmd.length(), 0);
        char buffer[1024] = {0};
        recv(tracker_socket, buffer, 1024, 0);
    }
    return true;
}


void handle_download(const string& command) {
    if (!ensure_connection()) {
        LOG("Cannot start download: No tracker available.");
        return;
    }
    
    stringstream ss(command);
    string action, group_id, filename, dest_path;
    ss >> action >> group_id >> filename >> dest_path;
    string request = "download_file " + group_id + " " + filename;
    send(tracker_socket, request.c_str(), request.length(), 0);
    char buffer[8192] = {0};
    ssize_t bytes_recvd = recv(tracker_socket, buffer, 8192, 0);

    if (bytes_recvd <= 0) {
        LOG("Connection to tracker lost during download initialization.");
        close(tracker_socket);
        tracker_socket = -1;
        return;
    }
    
    string response(buffer);
    stringstream resp_ss(response);
    string status;
    resp_ss >> status;
    if (status != "SUCCESS") { cout << response << endl; return; }
    LOG("Starting parallel download for " + filename);
    long long file_size;
    string file_sha, word;
    resp_ss >> file_size >> file_sha;
    vector<string> seeders;
    while(resp_ss >> word && word != "_END_SEEDERS_") seeders.push_back(word);
    vector<string> piece_hashes;
    while(resp_ss >> word) piece_hashes.push_back(word);
    if (seeders.empty()) { LOG("No seeders available for this file."); return; }
    int num_pieces = piece_hashes.size();
    if (num_pieces == 0 && file_size > 0) num_pieces = 1;
    string download_key = group_id + ":" + filename;
    {
        lock_guard<mutex> lock(g_downloads_mutex);
        g_downloads[download_key].group_id = group_id;
        g_downloads[download_key].filename = filename;
        g_downloads[download_key].total_pieces = num_pieces;
        g_downloads[download_key].completed_pieces = 0;
        g_downloads[download_key].is_complete = false;
    }
    vector<int> piece_indices;
    for (int i = 0; i < num_pieces; ++i) piece_indices.push_back(i);
    auto rd = random_device{};
    auto rng = mt19937{rd()};
    shuffle(piece_indices.begin(), piece_indices.end(), rng);
    atomic<size_t> next_piece_to_download{0};
    unsigned int num_threads = thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    num_threads = min((unsigned int)num_pieces, num_threads);
    LOG("Spawning " + to_string(num_threads) + " worker threads for download.");
    vector<thread> workers;
    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back([&]() {
            while (true) {
                size_t current_task_index = next_piece_to_download.fetch_add(1);
                if (current_task_index >= piece_indices.size()) break;
                int piece_index = piece_indices[current_task_index];
                string current_seeder = seeders[0];
                LOG("Downloading piece " + to_string(piece_index) + " from " + current_seeder);
                string temp_path = dest_path + ".part" + to_string(piece_index);
                if (download_piece_from_peer(current_seeder, filename, piece_index, temp_path)) {
                    g_downloads.at(download_key).completed_pieces++;
                } else {
                    LOG("Failed to download piece " + to_string(piece_index) + ". Re-scheduling.");
                    next_piece_to_download.fetch_sub(1);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    LOG("All pieces downloaded. Assembling file...");
    assemble_file(dest_path, num_pieces);
    if (get_file_sha1(dest_path) == file_sha) {
        LOG("Download completed for " + filename);
        LOG("[C] [" + group_id + "] " + filename);
        g_downloads.at(download_key).is_complete = true;
    } else {
        LOG("File integrity check failed. The downloaded file may be corrupt.");
    }
}


int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage: ./client <IP>:<PORT> <tracker_info.txt>" << endl;
        return 1;
    }
    my_listening_address = argv[1];
    size_t colon_pos = my_listening_address.find(':');
    int my_port = stoi(my_listening_address.substr(colon_pos + 1));
    thread(run_peer_server, my_port).detach();
    int tracker_info_fd = open(argv[2], O_RDONLY);
    if (tracker_info_fd < 0) { perror("open tracker_info.txt"); return 1; }
    char buffer[128];
    ssize_t n = read(tracker_info_fd, buffer, sizeof(buffer) - 1);
    if(n > 0) buffer[n] = '\0';
    close(tracker_info_fd);
    stringstream ss(buffer);
    ss >> g_tracker1_ip >> g_tracker1_port >> g_tracker2_ip >> g_tracker2_port;

    if (!connect_to_available_tracker()) {
        LOG("[ERROR] Could not connect to any tracker.");
        return 1;
    }
    
    string command_line;
    while (cout << "> " && getline(cin, command_line)) {
        if (command_line.empty()) continue;

        stringstream command_ss(command_line);
        string action;
        command_ss >> action;
        
        string final_command = command_line;

        if (action == "login") {
            string username, password;
            command_ss >> username >> password;
            g_current_user = username;
            g_current_password = password;
            final_command = "login " + username + " " + password + " " + my_listening_address;
        } else if (action == "quit") {
            break;
        }
        
        if (action != "create_user" && action != "login") {
             if (!ensure_connection()) {
                 continue;
             }
        }
        
        if (action == "upload_file") {
            string group_id, file_path;
            command_ss >> group_id >> file_path;
            struct stat file_stat;
            if (stat(file_path.c_str(), &file_stat) != 0) { LOG("File not found: " + file_path); continue; }
            string file_sha = get_file_sha1(file_path);
            string piece_hashes = get_piece_hashes(file_path);
            size_t last_slash = file_path.find_last_of('/');
            string filename = (last_slash == string::npos) ? file_path : file_path.substr(last_slash + 1);
            { lock_guard<mutex> lock(files_mutex); uploaded_files[filename] = file_path; }
            final_command = "upload_file " + group_id + " " + filename + " " + to_string(file_stat.st_size) + " " + file_sha + " " + piece_hashes;
        } else if (action == "download_file") {
            thread(handle_download, command_line).detach();
            continue; 
        } else if (action == "show_downloads") {
            lock_guard<mutex> lock(g_downloads_mutex);
            if (g_downloads.empty()) { cout << "No downloads to show." << endl; }
            else {
                for (const auto& pair : g_downloads) {
                    const auto& state = pair.second;
                    int completed = state.completed_pieces.load();
                    int total = state.total_pieces;
                    if (state.is_complete.load()) {
                        cout << "[C] [" << state.group_id << "] " << state.filename << endl;
                    } else {
                        int percentage = (total > 0) ? (completed * 100 / total) : 0;
                        cout << "[D] [" << state.group_id << "] " << state.filename << " ("
                                  << completed << "/" << total << " pieces) " << percentage << "%" << endl;
                    }
                }
            }
            continue;
        }

        final_command += "\n";
        if (send(tracker_socket, final_command.c_str(), final_command.length(), 0) <= 0) {
            LOG("Connection to tracker lost. Please enter your command again.");
            close(tracker_socket);
            tracker_socket = -1;
            continue;
        }

        char response_buffer[8192] = {0};
        if (recv(tracker_socket, response_buffer, 8192, 0) <= 0) {
            LOG("Connection to tracker lost. Please enter your command again.");
            close(tracker_socket);
            tracker_socket = -1;
            continue;
        }
        cout << response_buffer;
    }

    if (tracker_socket != -1) close(tracker_socket);
    return 0;
}