#!/usr/local/bin/ruby
require 'shared'

$0 = "sclient.rb"
done = 0
max = 1000
lock = Mutex.new

BlockSize = 16384
Clients = 10
ReadDelay = 0.10

FailRate = 0
WritesPerClient = (Content.size / BlockSize).ceil

workers = []
Clients.times do |i|
	workers << Thread.new do
		buf = ' ' * (BlockSize * 2)
		loop do
			lock.synchronize do
				i = (done += 1)
			end
			failclient = rand < FailRate
			failat = if failclient
				rand(WritesPerClient)
			else
				-1
			end
			w = 0

			puts "Iteration #{i}"
			sock = TCPSocket.new('127.0.0.1', 10200)
			amtread = 0
			sock.puts OutContent
		#	rbuf = ''
			while sock.read(BlockSize, buf)
				w += 1
				expected = Content[amtread...(amtread + buf.size)]
				if expected != buf
					puts "Buffer mismatch: #{buf} != #{expected}"
				end
				amtread += buf.size
				#puts "Read #{b.size} bytes, #{Content.size - amtread} bytes to go"
			#	rbuf << b
				if failat == w
					puts "Client bailing out randomly"
					break
				end
				sleep ReadDelay
			end
		#	if rbuf != Content[0...amtread]
		#		puts "Content returned modified!"
		#		p rbuf
		#	end
			sock.close
			puts "#{done} Complete"
			#lock.synchronize do
			#	puts "Syncing: #{done}"
				# done += 1
				raise "Done!" if done > max
			#end
		end
	end
end

workers.each {|w| w.join }
puts "Completed #{done} requests"

