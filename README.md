Peer-to-Peer File Sharing System

This project is a C++ implementation of a distributed, peer-to-peer file sharing system. It features a dual-tracker architecture for redundancy and enables clients to share and download files in parallel within private groups. 

Architectural Overview

The system follows a hybrid model, using centralized trackers for metadata management and decentralized, direct peer-to-peer connections for file transfers.

    Trackers: Two tracker servers run simultaneously, acting as the network's backbone. They do not store any file data. Their sole responsibilities are:

        Managing user accounts and authentication.

        Handling group creation, membership, and join requests.

        Storing metadata about shared files (size, hashes, and a list of seeders).

        Synchronizing all state changes with the peer tracker to provide high availability.

    Clients: The client application is the user's interface to the network. Each client is both a downloader (leecher) and an uploader (seeder).

        As a Leecher: When downloading, the client fetches file metadata from a tracker and then connects directly to other peers (seeders) to download file pieces in parallel.

        As a Seeder: Each client runs a background server thread that listens for incoming connections from other peers and serves requested file pieces.

        Failover: The client is designed to be resilient. If its connection to the primary tracker fails, it will automatically attempt to connect to the secondary tracker and re-establish its session, ensuring seamless operation.

Key Algorithms & Data Structures

Data Structures Rationale

    Tracker-Side (std::map): The tracker heavily relies on std::map for storing users, groups, and file metadata. This provides efficient O(log N) key-based lookups, which is ideal for retrieving user credentials, group details, or file information based on their unique IDs.

    Thread Safety (std::mutex): All access to shared data structures on the multi-threaded tracker and client is protected by std::mutex. This prevents race conditions and ensures data integrity when multiple clients or threads interact with the system simultaneously.

    Client-Side Download Tracking (std::map<string, DownloadState>): A global map tracks the state of all ongoing and completed downloads. This allows the show_downloads command to instantly report progress.

    Progress Counters (std::atomic): Within each DownloadState, std::atomic<int> and std::atomic<bool> are used for completed_pieces and is_complete. Atomics provide lock-free, thread-safe updates, which is highly efficient for frequent updates from multiple download threads.

Algorithms Implemented

    Tracker Synchronization: The system uses an active-push synchronization model. Whenever a state-changing operation (e.g., creating a user, accepting a group request, uploading a file) occurs on one tracker, it immediately constructs a SYNC command and pushes it to its peer. The peer tracker receives this command and applies the same state change to its own data structures, ensuring both trackers remain consistent.

    Parallel Downloading (Piece Selection): To achieve non-sequential, parallel downloads, the client implements a randomized worker pool strategy:

        A list of all required piece indices (0, 1, 2...N-1) is created.

        This list is shuffled into a random order using <random>.

        A pool of worker threads (sized based on std::thread::hardware_concurrency()) is created.

        Threads concurrently pull piece indices from the shuffled list using a shared std::atomic counter. This is more efficient than a mutex-locked queue for a fixed number of tasks.

        This ensures that multiple, random pieces of the file are downloaded simultaneously, maximizing throughput.

    SHA1 Integrity Verification: File integrity is guaranteed using SHA1 hashes. The client pre-calculates the hash of the entire file and of each 512KB piece before uploading. A downloading client receives these expected hashes from the tracker and verifies the final assembled file against its hash.

Network Protocol & Message Formats

All communication is plaintext over TCP/IP sockets. Messages are space-delimited and terminated by a newline character (\n).

Client-Tracker Protocol

    create_user <user_id> <password>

    login <user_id> <password> <listening_ip:port>

    create_group <group_id>

    join_group <group_id>

    list_requests <group_id>

    accept_request <group_id> <user_id>

    list_groups

    logout

    upload_file <group_id> <filename> <filesize> <file_sha1> [piece_sha1_1] [piece_sha1_2] ...

    list_files <group_id>

    download_file <group_id> <filename>

    show_downloads

Tracker-Tracker Protocol

All synchronization commands are prefixed with SYNC.

    SYNC CREATE_USER <user_id> <password>

    SYNC LOGIN <user_id> <listening_ip:port>

    ...and so on for all other state-changing commands.

Client-Client Protocol

The peer-to-peer protocol is minimal, consisting of a single request type.

    GET_PIECE <filename> <piece_index>

Compilation & Execution

Prerequisites

    A C++17 compatible compiler (g++)

    make

    OpenSSL development libraries (libssl-dev on Debian/Ubuntu, or openssl-devel on Fedora/CentOS)

Compilation

Navigate to each directory and run make.
Bash

# Compile the tracker
cd tracker/
make

# Compile the client
cd ../client/
make

Execution

    Configure Trackers: Create a tracker_info.txt file in your root project directory with the IP and port of each tracker, one per line.

    127.0.0.1 8080
    127.0.0.1 8081

    Start Trackers: Open two separate terminals to run the two tracker instances.
    Bash

# Terminal 1: Start Tracker 1
./tracker/tracker tracker_info.txt 1

# Terminal 2: Start Tracker 2
./tracker/tracker tracker_info.txt 2

Start Clients: Open one or more terminals to run client instances. Each client must be started with its own unique <IP>:<PORT> for peer listening.
Bash

    # Terminal 3: Start Client A
    ./client/client 127.0.0.1:5001 tracker_info.txt

    # Terminal 4: Start Client B
    ./client/client 127.0.0.1:5002 tracker_info.txt

Implemented Features

    Full User & Group Management: All specified commands (create_user, login, create_group, join_group, etc.) are implemented.

    Dual-Tracker Redundancy: The system remains operational as long as at least one tracker is running.

    Automatic Client Failover: Clients seamlessly reconnect to the secondary tracker if the primary fails, including automatic re-authentication.

    File Sharing: Uploading, listing, and downloading files within groups.

    Parallel Downloads: File pieces are downloaded concurrently using a multi-threaded worker pool for increased speed.

    Non-Sequential Piece Selection: The order of piece downloads is randomized to simulate a more realistic P2P download pattern.
s
    Download Progress Tracking: The show_downloads command provides real-time status of ongoing and completed downloads.

    File Integrity: SHA1 hashing is used to verify the integrity of downloaded files.

Assumptions & Limitations

    Assumptions:

        Filenames are assumed to be unique within a given group.

        The tracker_info.txt file is correctly formatted and accessible.

        Clients are not behind restrictive NATs that would prevent direct peer-to-peer connections (UPnP/NAT traversal is not implemented).

    Limitations:

        Single Seeder Downloads: While pieces are downloaded in parallel, the current client implementation downloads all pieces for a file from a single seeder (the first one returned by the tracker). A more advanced implementation would connect to multiple seeders for the same file.

        Basic Piece Strategy: The piece selection strategy is random shuffling. It does not implement a "rarest-first" algorithm, which would be more efficient in a swarm with many leechers and partial seeders.

        Security: Communication is not encrypted, and passwords are sent in plaintext.

        Error Handling: The protocol relies on simple "SUCCESS" or "ERROR" messages. It does not have a rich set of error codes for different failure scenarios.

Testing Procedures

1. Basic Functionality Test

    Setup: Start both trackers and two clients (Client A and Client B).

    User/Group Setup:

        On Client A: create_user user_a pass, login user_a pass, create_group testgroup.

        On Client B: create_user user_b pass, login user_b pass, join_group testgroup.

        On Client A: list_requests testgroup (to see the request), accept_request testgroup user_b.

    File Transfer:

        On Client A: Create a sample file (e.g., dummy.txt) and run upload_file testgroup dummy.txt.

        On Client B: list_files testgroup (to see the file), then download_file testgroup dummy.txt downloaded_dummy.txt.

    Verification: Check that downloaded_dummy.txt exists and its content is identical to the original dummy.txt. Use diff or sha1sum to verify.

2. Tracker Failover Test

    Setup: Perform steps 1 and 2 from the Basic Functionality Test.

    Initiate Action: From Client A, run a command like list_groups. It should succeed.

    Simulate Crash: Go to the terminal of the primary tracker (the one the clients first connected to) and shut it down (Ctrl+C).

    Test Failover: Immediately run another command on Client A, for example, list_groups again.

    Observe: The client terminal should log "Connection lost," then "Attempting to reconnect," and finally "Successfully connected to secondary tracker." The list_groups command should then execute successfully, retrieving its data from the secondary tracker without any user intervention.
