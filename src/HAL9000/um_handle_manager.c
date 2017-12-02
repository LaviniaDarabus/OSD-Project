#include "HAL9000.h"
#include "um_handle_manager.h"
#include "intutils.h"

UM_HANDLE
generateUmHandle(
	IN HandleId id,
	IN QWORD handleLow
) {
	//genereze random handle pt fisiere
	//folosim DWORDS_TO_QWORD(x,y) pt handle de proces si tid
}