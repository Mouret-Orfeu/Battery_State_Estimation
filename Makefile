CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -Iinclude -lm -g

SRC     = src/soc_coulomb.c src/soc_ocv.c src/soc_ekf.c
TEST_CC = test/test_coulomb.c $(SRC)

.PHONY: all test simulate clean

all:
	$(CC) $(CFLAGS) $(SRC) -o bms_soc_demo

test: test_coulomb

test_coulomb:
	$(CC) $(CFLAGS) $(TEST_CC) -o test_coulomb_out
	./test_coulomb_out

simulate:
	python3 scripts/simulate_cell.py --capacity 60 --duration 3600 --output docs/test_vectors.csv
	@echo "Simulation complete — see docs/test_vectors.csv"

clean:
	rm -f bms_soc_demo test_coulomb_out *.o docs/test_vectors.csv
