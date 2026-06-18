#  Concurrent Banking System (OS Simulation)

##  Overview
A C-based Operating System simulation that demonstrates core OS concepts using a banking system model.

##  Features
- CPU Scheduling (FCFS, Priority, Round Robin)
- Synchronization (Mutex, Semaphore)
- Deadlock Avoidance (Banker’s Algorithm)
- IPC (Producer-Consumer, Pipes, Threads)
- Memory Management (FIFO, LRU)

##  Concepts Used
- Scheduling Algorithms
- Race Conditions & Synchronization
- Deadlock Handling
- Inter-Process Communication
- Page Replacement

##  How to Run
gcc -std=c11 src/*.c -o bank_os_sim -pthread  
./bank_os_sim

##  Structure
- scheduler.c  
- synchronization.c  
- ipc.c  
- memory.c  
- main.c  

##  Author
Meerab Fatima
