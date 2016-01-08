
all:
	gcc -Wall -g ccvm.c -o ccvm -pthread

clean:
	rm -f ccvm
