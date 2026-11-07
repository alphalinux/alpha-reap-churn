CFLAGS ?= -O2
LDLIBS += -pthread

churn: churn.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f churn

.PHONY: clean
