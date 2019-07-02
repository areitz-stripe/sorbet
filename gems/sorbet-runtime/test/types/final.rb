# frozen_string_literal: true
require_relative '../test_helper'

class Opus::Types::Test::FinalTest < Critic::Unit::UnitTest
  after do
    T::Private::DeclState.current.reset!
  end

  it "allows declaring an instance method as final" do
    Class.new do
      extend T::Sig
      sig {void}.final
      def foo; end
    end
  end

  it "allows declaring a class method as final" do
    Class.new do
      extend T::Sig
      sig {void}.final
      def self.foo; end
    end
  end

  it "forbids redefining a final instance method" do
    err = assert_raises(RuntimeError) do
      Class.new do
        extend T::Sig
        sig {void}.final
        def foo; end
        sig {void}.final
        def foo; end
      end
    end
    assert_includes(err.message, "was declared as final and cannot be redefined")
  end

  it "forbids redefining a final class method" do
    err = assert_raises(RuntimeError) do
      Class.new do
        extend T::Sig
        sig {void}.final
        def self.foo; end
        sig {void}.final
        def self.foo; end
      end
    end
    assert_includes(err.message, "was declared as final and cannot be redefined")
  end

  it "forbids overriding a final instance method" do
    c = Class.new do
      extend T::Sig
      sig {void}.final
      def foo; end
    end
    err = assert_raises(RuntimeError) do
      Class.new(c) do
        def foo; end
      end
    end
    assert_includes(err.message, "was declared as final and cannot be overridden")
  end

  it "forbids overriding a final class method" do
    c = Class.new do
      extend T::Sig
      sig {void}.final
      def self.foo; end
    end
    err = assert_raises(RuntimeError) do
      Class.new(c) do
        def self.foo; end
      end
    end
    assert_includes(err.message, "was declared as final and cannot be overridden")
  end
end
