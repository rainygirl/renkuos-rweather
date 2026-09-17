NAME     = rweather
CXX     ?= g++
CXXFLAGS = -Wall -Wno-multichar -O2
LIBS     = -lbe -lbnetapi -lnetwork

$(NAME): rweather.cpp rweather.rdef
	$(CXX) $(CXXFLAGS) -o $(NAME) rweather.cpp $(LIBS)
	rc -o $(NAME).rsrc rweather.rdef
	xres -o $(NAME) $(NAME).rsrc
	mimeset -f $(NAME)

BIN = $(HOME)/config/non-packaged/bin

# Add the tray item now (also what you run once to "install" it).
install: $(NAME)
	./$(NAME)

# Remove it from the tray.
uninstall: $(NAME)
	./$(NAME) --remove

# Deploy to a permanent location and (re)add to the tray. IMPORTANT: Deskbar
# memory-maps the executable while the replicant is loaded, so overwriting it
# in place corrupts the running copy and crashes Deskbar. Always remove it
# first (unloading the add-on), then copy, then add.
deploy: $(NAME)
	-test -x $(BIN)/$(NAME) && $(BIN)/$(NAME) --remove || true
	mkdir -p $(BIN)
	cp $(NAME) $(BIN)/$(NAME)
	$(BIN)/$(NAME)

clean:
	rm -f $(NAME) $(NAME).rsrc

.PHONY: install uninstall deploy clean
