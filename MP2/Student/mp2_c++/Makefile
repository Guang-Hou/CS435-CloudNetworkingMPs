all: main ls

ls: lsPeriodicNewShare.cpp 
	  g++ lsPeriodicNewShare.cpp -pthread -o ls

pv: pv_periodicNewShare.cpp 
	  g++ pv_PeriodicNewShare.cpp -pthread -o pv

.PHONY: clean

clean:
	rm ls pv log1 log2 log3 