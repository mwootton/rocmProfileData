PYTHON ?= python3

.PHONY:
all: rpd rocpd

.PHONY: install
install:
	$(MAKE) install -C rocpd_python
	$(MAKE) install -C rpd_tracer
	$(MAKE) install -C rpd_dash

.PHONY: uninstall
uninstall:
	$(MAKE) uninstall -C rocpd_python
	$(MAKE) uninstall -C rpd_tracer
	$(MAKE) uninstall -C rpd_dash

.PHONY: clean
clean:
	$(MAKE) clean -C rocpd_python
	$(MAKE) clean -C rpd_tracer
	$(MAKE) clean -C rpd_dash

rpd:
	$(MAKE) -C rpd_tracer

rocpd:
	$(MAKE) -c rocpd_python

