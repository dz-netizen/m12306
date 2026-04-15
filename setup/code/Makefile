CC=g++
CFLAGS=-g -Wall
LIBS=-lcgicc -lpq


TARGET= cgi_get.cgi practice2.cgi login.cgi register.cgi home.cgi query_train.cgi query_route.cgi book.cgi orders.cgi admin.cgi

all: ${TARGET}

%.cgi: %.cpp
	${CC} ${CFLAGS} -o $@ $< ${LIBS}


cgi_get.cgi:   cgi_get.cpp 
practice2.cgi: practice2.cpp
login.cgi: login.cpp
register.cgi: register.cpp
home.cgi: home.cpp
query_train.cgi: query_train.cpp
query_route.cgi: query_route.cpp
book.cgi: book.cpp
orders.cgi: orders.cpp
admin.cgi: admin.cpp

clean:
	-rm -f a.out ${TARGET}

