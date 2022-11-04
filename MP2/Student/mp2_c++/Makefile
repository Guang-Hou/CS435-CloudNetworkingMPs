all: main ls

pv: main.cpp manager_send.c
	gcc manager_send.c -o manager_send 
	g++ main.cpp -o main -pthread

ls_periodic: ls_periodic.cpp manager_send.c
	  gcc manager_send.c -o manager_send 
	  g++ ls_periodic.cpp -pthread -o ls_periodic

ls_share: ls_share.cpp manager_send.c
	  gcc manager_send.c -o manager_send 
	  g++ ls_share.cpp -pthread -o ls_share

.PHONY: clean
clean:
	rm manager_send log1 log2 log3 ls_share ls_periodic
