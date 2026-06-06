#include <string>

class ClientData{
public:
    ClientData(int fd,const std::string& _name) : fd(fd), name(_name) {}

    void PrintInfo() const{
        const std::string banString = (bIsBanned) ? "banned" : "not banned";
        const std::string adminString = (bIsAdmin) ? "is admin" : "is not admin";
        printf("Name: %s , FD: %d , Ban Status: %s , Admin Status: %s \n",name.c_str(),fd,banString.c_str(),adminString.c_str());
    };

    // Getters

    int get_fd() const { return fd; }

    std::string get_name() const { return name; }

    bool GetIsBanned() const {return bIsBanned;}

    bool GetIsAdmin() const {return bIsAdmin;}

    // Setters

    bool SetIsBanned(bool bBanned){
        if(bIsBanned == true)
        {
            printf("%s is allready banned, cannot ban twice\n",name.c_str());
            return false;
        }

        bIsBanned = bBanned;
        return bIsBanned;
    }

private:
    int fd;
    std::string name;
    bool bIsBanned{false};
    bool bIsAdmin{false};
};