CC=g++
CFLAGS=-g -Wall
LIBS=-lcgicc -lpq
SRC_DIR=src
BUILD_DIR=build

TARGET=$(addprefix $(BUILD_DIR)/, login.cgi register.cgi home.cgi query_train.cgi query_route.cgi book.cgi orders.cgi admin.cgi)

all: $(BUILD_DIR) ${TARGET}

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.cgi: $(SRC_DIR)/%.cpp
	${CC} ${CFLAGS} -o $@ $< ${LIBS}

clean:
	-rm -rf $(BUILD_DIR)
