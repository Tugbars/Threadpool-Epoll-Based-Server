# Network Programming with Non-Blocking Sockets, epoll, and ThreadPool: A Hybrid Approach

## Overview

This repository contains a C++ application designed to efficiently manage client connections and MySQL database interactions using a hybrid approach that combines non-blocking sockets, `epoll`, and a thread pool. This design ensures scalability, efficiency, and simplicity in handling numerous client connections and database operations. Each connection is supported by one thread, but with a few tweaks, this can be scaled up to handle multiple connections per thread, leveraging the full potential of the thread pool and `epoll`.

The server is based on the TCP communication protocol, utilizes heartbeats for client connectivity management, and supports a dual-stack IP approach (IPv4 and IPv6).

## Design Decisions

### One Thread per Socket

#### Advantages:

- **Simplicity**: Each thread handles a single connection. This is easy to understand and implement. Each thread can use blocking I/O, and there's no need to worry about non-blocking I/O, `epoll`, or complex event handling.
- **Isolation**: Each connection is isolated in its own thread. Issues in one connection (e.g., blocking operations, long-running tasks) don't directly affect other connections.

#### Disadvantages:

- **Resource Intensive**: Threads consume significant system resources. Each thread has a stack, and there is overhead in context switching between threads.
- **Scalability**: This model doesn't scale well with a large number of connections. Creating thousands of threads can lead to high memory usage and context-switching overhead.
- **Complexity in Synchronization**: If threads need to share data, you must implement synchronization mechanisms (mutexes, locks), which can become complex and can introduce bottlenecks.

### Using `epoll` with Non-Blocking Sockets

#### Advantages:

- **Efficiency**: One or a few threads can handle many connections. This significantly reduces memory usage and context-switching overhead.
- **Scalability**: This model scales much better for a large number of connections. The kernel efficiently manages which file descriptors are ready for I/O.
- **Event-Driven**: The event-driven model is well-suited for handling many simultaneous I/O operations, especially when connections are idle most of the time.

#### Disadvantages:

- **Complexity**: Requires more complex code to manage non-blocking I/O, `epoll` events, and state management for each connection.
- **Single Point of Failure**: A bug in the event loop can potentially affect handling multiple connections.

### Performance Considerations

#### Performance of One Thread per Socket:

- Creating and destroying threads is expensive.
- High memory usage due to stack space for each thread.
- High context-switching overhead when many threads are active.
- Can be easier to implement and debug for a small number of connections.

#### Performance of `epoll` with Non-Blocking Sockets:

- Lower memory usage and context-switching overhead.
- Better scalability for high concurrency.
- Requires careful handling of state and events, but benefits from efficient I/O operations.

## Combining Approaches: Hybrid Model

In practice, combining both approaches can provide a balance:

- **Thread Pool with `epoll`**: Use a limited number of worker threads and a central `epoll` instance to manage I/O. When a file descriptor is ready for I/O, dispatch the work to a thread pool. This combines the scalability of `epoll` with the simplicity of threads.
- **Hybrid Model**: Use threads to handle computationally intensive tasks while using `epoll` to handle I/O readiness. This allows efficient I/O management while offloading heavy tasks to threads.

## Implementation Details

### Server Code

The server utilizes non-blocking sockets and `epoll` for efficient I/O multiplexing, combined with a thread pool to handle client connections and perform tasks asynchronously. Each connection is supported by one thread, but this design can be scaled up to handle multiple connections per thread with a few adjustments.

#### Key Components:

- **Server Class**: Manages the main server operations, including initializing the server, handling new connections, and managing the `epoll` instance.
- **Connection Handler**: Handles individual client connections in separate threads, processing incoming messages and maintaining the connection state.
- **Thread Pool**: Manages a pool of worker threads to execute tasks. Supports dynamic resizing, pausing, and resuming.
- **TCP Communication**: Utilizes TCP for reliable data transmission.
- **Heartbeat Mechanism**: Sends periodic heartbeat messages to maintain client connectivity.
- **Dual-Stack IP Support**: Supports both IPv4 and IPv6, ensuring broader compatibility and future-proofing.

### MySQL Connection Pool

The MySQL connection pool efficiently manages database connections, ensuring thread safety and adhering to RAII principles.

#### Key Features:

- **Connection Pool Management**: Dynamically maintains a pool of active database connections for reuse, minimizing the performance overhead of establishing connections.
- **Thread Safety**: Ensures that database connections can be safely accessed and modified by multiple threads, using `std::mutex` for synchronization.
- **RAII Compliance**: Automates resource management, ensuring that all resources (e.g., database connections) are properly released when no longer needed.
- **Health Check Mechanism**: Regularly verifies the validity of connections using the `isConnectionValid` method, replacing any invalid connections to maintain the pool's integrity.

## Server Implementation

The server class initializes the server, sets up the `epoll` instance, and manages incoming connections. It uses non-blocking sockets and `epoll` to handle I/O events efficiently, dispatching tasks to a thread pool for processing.

### Server Class Methods:

- **initialize**: Sets up the listening socket and configures it for non-blocking mode. Initializes the `epoll` instance and adds the listening socket to the `epoll` interest list.
- **start**: Begins the main server loop, handling new connections and dispatching events to the appropriate handlers.
- **handleNewConnection**: Accepts new client connections and adds them to the `epoll` instance. Each connection is handled in a separate thread from the thread pool.
- **adjustThreadPoolSize**: Dynamically adjusts the size of the thread pool based on the current workload, ensuring optimal resource utilization.

### MySQL Connection Pool Implementation

The `MySqlConnectionPool` class manages a pool of connections to the MySQL database, providing efficient reuse of connections and ensuring thread safety.

### MySqlConnectionPool Class Methods:

- **getConnection**: Retrieves an available connection from the pool, validating its health before returning it for use.
- **returnConnection**: Returns a connection to the pool after use, replacing it if it's no longer valid.
- **createConnection**: Initializes the pool with a specified number of connections and ensures they are valid.
- **isConnectionValid**: Checks the health of a connection by executing a simple query.
- **createNewConnection**: Establishes a new connection to the MySQL database, retrying on failure.

### MySqlConnectionManager Class

The `MySqlConnectionManager` class initializes the connection pool and provides methods to get and return connections. It also ensures the database schema is set up correctly by creating necessary tables.

## Usage

To use this server and connection pool in your application, include the provided classes and configure the server and database settings. Start the server and manage connections and database interactions through the provided interfaces.

## Compilation and Dependencies

This project requires the MySQL Connector/C++ to interact with MySQL databases. Ensure that the connector is installed and properly configured in your build environment. Compile the application with a C++ compiler that supports C++11 or later.
