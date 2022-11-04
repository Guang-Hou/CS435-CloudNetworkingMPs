all: main ls

pv: main.cpp manager_send.c
	gcc manager_send.c -o manager_send 
	g++ main.cpp -o main -pthread

ls_p: ls_periodic.cpp manager_send.c
	  gcc manager_send.c -o manager_send 
	  g++ ls_periodic.cpp -pthread -o ls_periodic

.PHONY: clean
clean:
	rm manager_send log1 log2 log3 ls_periodic
