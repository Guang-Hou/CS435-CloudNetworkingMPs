all: main ls

main: main.cpp manager_send.c
	gcc manager_send.c -o manager_send 
	g++ main.cpp -o main -pthread

ls: main.c monitor_neighbors.c
	gcc -pthread -o ls_router main.c monitor_neighbors.c

.PHONY: clean
clean:
	rm main manager_send
