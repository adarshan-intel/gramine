# Disable implicit rules so that Make doesn't try to compile the binaries
.SUFFIXES:

.PHONY: all
all:
	@echo "Use gramine-test for running tests."

# TODO: Python tests should not produce leftovers
.PHONY: clean
clean:
	gramine-test clean
	$(RM) -r \
		*.dat \
		.pytest_cache \
		__pycache__ \
		*.xml \
		testfile \
		trusted_testfile \
		asm/x86_64/__pycache__ \
		tmp/*
	find tmp_enc/ -not -path '*/.*' -type f -delete
