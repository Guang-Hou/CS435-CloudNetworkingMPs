all: main ls

ls: lsPeriodicNewShare.cpp 
	  g++ -pthread -g -std=c++11 -o ls lsPeriodicNewShare.cpp 

pv: pv_periodicNewShare.cpp 
	  g++ pv_PeriodicNewShare.cpp -pthread -o pv

.PHONY: clean

clean:
	rm ls pv log1 log2 log3 