TTFFILES=	urw-palladio-l-roman.ttf \

all:	generate ebook-red-letter.profile $(TTFFILES)
	./generate ebook-red-letter.profile

generate:	generate.c
	gcc -g -Wall -I/usr/local/include -o generate generate.c -L/usr/local/lib -lhpdf

clean:
	rm *.pdf *.toc *.aux *.log *.fls
