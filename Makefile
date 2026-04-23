# Use 'make help' to see available targets.

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -std=c99
CFLAGS  += -Iinclude
LDLIBS  ?= -lm

SRCDIR  = src
TESTDIR = test
OBJDIR  = obj
BINDIR  = bin
SCRIPTS = scripts

SOURCES := $(wildcard $(SRCDIR)/*.c)
OBJECTS := $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

$(OBJECTS): $(OBJDIR)/%.o : $(SRCDIR)/%.c
	mkdir -p $(OBJDIR)
	$(CC) -o $@ -c $< $(CFLAGS)

.PHONY: clean help debug valgrind test_all test_coulomb test_ekf simulate

# Run all test suites — add new test_* targets here as dependencies
test_all: test_coulomb test_ekf

test_coulomb: $(BINDIR)/test_coulomb_out
	@echo "Running Coulomb counting tests..."
	./$(BINDIR)/test_coulomb_out

$(BINDIR)/test_coulomb_out: $(TESTDIR)/test_coulomb.c $(OBJECTS)
	mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
	@echo "Coulomb counting test build complete!"

test_ekf: $(BINDIR)/test_ekf_out
	@echo "Running EKF tests..."
	./$(BINDIR)/test_ekf_out

$(BINDIR)/test_ekf_out: $(TESTDIR)/test_ekf.c $(OBJECTS)
	mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
	@echo "EKF test build complete!"

simulate:
	python3 $(SCRIPTS)/simulate_cell.py --capacity 60 --duration 3600 --output docs/test_vectors.csv
	@echo "Simulation complete — see docs/test_vectors.csv"

clean:
	@echo "Cleaning up..."
	rm -rf $(OBJDIR) $(BINDIR) docs/test_vectors.csv
	@echo "Clean complete!"

help:
	@echo "Available targets:"
	@echo "  make clean         - Remove all generated files"
	@echo "  make debug         - Build and run all tests with debug flags"
	@echo "  make valgrind      - Run all tests under Valgrind (memory check)"
	@echo "  make test_all      - Build and run all test suites"
	@echo "  make test_coulomb  - Build and run only the Coulomb counting tests"
	@echo "  make test_ekf      - Build and run only the EKF tests"
	@echo "  make simulate      - Run cell simulation script, generate current-SoC time series"
	@echo "  make help          - Show this help message"

debug: CFLAGS += -g -O0
debug: $(BINDIR)/test_coulomb_out
	gdb ./$(BINDIR)/test_coulomb_out

valgrind: CFLAGS += -g -O0
valgrind: $(BINDIR)/test_coulomb_out
	@echo "Running Coulomb counting tests under Valgrind..."
	valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all ./$(BINDIR)/test_coulomb_out
