# typed: true
# disable-fast-path: true
# Found by fuzzer: https://github.com/sorbet/sorbet/issues/1132
module MyEnumerable
extend T::Generic
A = type_member
  sig {params(a: MyEnumerable[])} # error: Malformed `sig`: No return type specified. Specify one with .returns()
# ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ error: Method `sig` does not exist on `T.class_of(MyEnumerable)`
     # ^^^^^^^^^^^^^^^^^^^^^^^^^ error: Method `params` does not exist on `T.class_of(MyEnumerable)`
               # ^^^^^^^^^^^^^^ error-with-dupes: Wrong number of type parameters for `MyEnumerable`.
  def-(a) end
 class MySet
include MyEnumerable
 A = # error: Type variable `A` needs to be declared as `= type_member(SOMETHING)`
new-MySet.new()  end
end
