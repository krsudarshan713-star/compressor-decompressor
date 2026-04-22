# HUFFMAN Compression Method

**Prerequisites**  
gcc

**Compilation**  
```bash
gcc Huffman.c -o huff
```

**To Run the program**  
* To compress <original_file> and save it in an <encoded_file>:
```bash
./huff c <original_file>.txt <encoded_file>.huf
```
* To decompress <encoded_file> file and save it in an <restored_file>:
```bash
./huff d <encoded_file>.huf <restored_file>.txt
```

* To compare if the files are same:
```bash
./huff v <file1> <file2>
```

*To show the frequency table:
```bash
./huff i <encoded_file>.huf
```

*To print table with savings per character:
```bash
./huff p <original_file>.txt
```
