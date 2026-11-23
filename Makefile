# SPDX-License-Identifier: AGPL-3.0-or-later
# Every app in apps/, and the feed. Needs T48_SDK (see README.md).
#   make            build every app (for the phone and for the simulator)
#   make check      run each app's tests/*.script in the simulator
#   make feed       package every app into site/ with index.json and a page
APPS := $(patsubst apps/%/Makefile,%,$(wildcard apps/*/Makefile))
SITE := site

all check:
	@for a in $(APPS); do $(MAKE) -C apps/$$a $@ || exit 1; done

feed: all
	rm -rf ./$(SITE)
	mkdir -p $(SITE)/screenshots
	@for a in $(APPS); do $(MAKE) -s -C apps/$$a package >/dev/null || exit 1; \
		cp apps/$$a/build/*.t48app $(SITE)/; \
		for p in apps/$$a/screenshots/*.png; do [ -f "$$p" ] && cp "$$p" $(SITE)/screenshots/$$a-$$(basename $$p); done; done
	$(T48_SDK)/bin/t48-pack --index $(SITE)
	tools/feed-page.py $(SITE) > $(SITE)/index.html

clean:
	@for a in $(APPS); do $(MAKE) -C apps/$$a clean; done
	rm -rf ./$(SITE)

.PHONY: all check feed clean
