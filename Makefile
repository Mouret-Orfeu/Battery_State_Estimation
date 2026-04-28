# Use 'make help' to see available targets.

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -std=c99
CFLAGS  += -Iinclude
LDLIBS  ?= -lm

SRCDIR  = src
TESTDIR = test
OBJDIR  = obj
BINDIR  = bin
LIBDIR  = lib
SCRIPTS = scripts

SOURCES := $(wildcard $(SRCDIR)/*.c)
OBJECTS := $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

$(OBJECTS): $(OBJDIR)/%.o : $(SRCDIR)/%.c
	mkdir -p $(OBJDIR)
	$(CC) -o $@ -c $< $(CFLAGS)

.PHONY: clean help debug valgrind test_all test_coulomb test_ekf test_ocv simulate lib

# Run all test suites — add new test_* targets here as dependencies
test_all: test_coulomb test_ekf test_ocv

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

test_ocv: $(BINDIR)/test_ocv_out
	@echo "Running OCV lookup tests..."
	./$(BINDIR)/test_ocv_out

$(BINDIR)/test_ocv_out: $(TESTDIR)/test_ocv.c $(OBJECTS)
	mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
	@echo "OCV lookup test build complete!"

lib:
	mkdir -p $(LIBDIR)
	$(CC) $(CFLAGS) -fPIC -shared -o $(LIBDIR)/libbms.so $(SOURCES) $(LDLIBS)
	@echo "Shared library built → $(LIBDIR)/libbms.so"

simulate:
	python3 $(SCRIPTS)/simulate_cell.py --capacity 60 --duration 3600 --output docs/simulated_cell_behavior.csv
	@echo "Simulation complete — see docs/simulated_cell_behavior.csv"

clean:
	@echo "Cleaning up..."
	rm -rf $(OBJDIR) $(BINDIR) $(LIBDIR)
	@echo "Clean complete!"

help:
	@echo "Available targets:"
	@echo "  make clean                  - Remove all generated files"
	@echo "  make debug                  - Build and run all tests with debug flags"
	@echo "  make valgrind_test_coulomb  - Run Coulomb counting tests under Valgrind (memory check)"
	@echo "  make valgrind_test_ekf      - Run EKF tests under Valgrind (memory check)"
	@echo "  make valgrind_test_ocv      - Run OCV lookup tests under Valgrind (memory check)"
	@echo "  make test_all      	     - Build and run all test suites"
	@echo "  make test_coulomb  		 - Build and run only the Coulomb counting tests"
	@echo "  make test_ekf      	     - Build and run only the EKF tests"
	@echo "  make test_ocv      	     - Build and run only the OCV lookup tests"
	@echo "  make simulate               - Run cell simulation, generate current-SoC time series"
	@echo "  make help          		 - Show this help message"

debug: CFLAGS += -g -O0
debug: $(BINDIR)/test_coulomb_out
	gdb ./$(BINDIR)/test_coulomb_out

valgrind_test_coulomb valgrind_test_ekf valgrind_test_ocv: CFLAGS += -g -O0

valgrind_test_coulomb: $(BINDIR)/test_coulomb_out
	@echo "Running Coulomb counting tests under Valgrind..."
	valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all ./$(BINDIR)/test_coulomb_out

valgrind_test_ekf: $(BINDIR)/test_ekf_out
	@echo "Running EKF tests under Valgrind..."
	valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all ./$(BINDIR)/test_ekf_out

valgrind_test_ocv: $(BINDIR)/test_ocv_out
	@echo "Running OCV lookup tests under Valgrind..."
	valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all ./$(BINDIR)/test_ocv_out