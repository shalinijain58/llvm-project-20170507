
void %test(sbyte* %P, short* %P, int* %P) {
	%V = load sbyte* %P
	store sbyte %V, sbyte* %P

	%V = load short* %P
	store short %V, short* %P

	%V = load int* %P
	store int %V, int* %P
	ret void
}

uint %varalloca(uint %Size) {
	%X = alloca uint, uint %Size        ;; Variable sized alloca
	store uint %Size, uint* %X
	%Y = load uint* %X
	ret uint %Y
}

int %main() {
	%A = alloca sbyte
	%B = alloca short
	%C = alloca int
	call void %test(sbyte* %A, short* %B, int* %C)
	call uint %varalloca(uint 7)

	ret int 0
}
