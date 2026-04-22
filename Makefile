CC = gcc

huff: Huffman.c
	$(CC) -o huff Huffman.c

clean:
	rm -f huff
