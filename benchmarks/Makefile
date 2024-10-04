BENCHMARKS := $(shell find . -mindepth 2 -maxdepth 2 -type d -not -path "./data/*" -not -path "./Results/*")

default:
	for dir in $(BENCHMARKS) ; do make -C $$dir; done

clean:
	for dir in $(BENCHMARKS) ; do make clean -C $$dir; done
