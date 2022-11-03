all: main ls

pv: main.cpp manager_send.c
	gcc manager_send.c -o manager_send 
	g++ main.cpp -o main -pthread

ls: ls.cpp manager_send.c
	gcc manager_send.c -o manager_send 
	g++ ls.cpp -pthread -o ls

.PHONY: clean
clean:
	rm manager_send log1 log2 log3 ls
