//
// Created by Eminf on 19.07.2026.
//

#ifndef TYPEKIND_H
#define TYPEKIND_H



enum class TypeKind {
 Int8,
 Int16,
 Int32,
 Int64,
 UInt8,
 UInt16,
 UInt32,
 UInt64,
 Float32,
 Float64,
 Bool,
 Char,
 String,
 Void,
 Array,
 List,
 Matrix,
 Lambda,
 Named   // kullanıcının tanımladığı tip (struct); adı Type::name alanında
};



#endif //TYPEKIND_H
